/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minisforum Hardware Monitoring Driver (HWMON)
 *
 * Exposes temperatures, fan speeds, PWM manual control, and an in-kernel
 * composite thermal governor with stepped rate-limited EC writes.
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include "minisforum.h"

/* IT5571 Tachometer Register Addresses */
#define MF_REG_TACH_CPU_LO	0x181E
#define MF_REG_TACH_CPU_HI	0x181F
#define MF_REG_TACH_SYS_LO	0x1820
#define MF_REG_TACH_SYS_HI	0x1821
#define MF_TACH_FREQ_HZ		2156250

/* IT5571 CPU Fan Curve Lookup Table Addresses (XRAM) */
#define MF_REG_CPU_BASE_START	0x0310
#define MF_REG_CPU_SLOPE_BASE	0x08B0
#define MF_MAX_FAN_CODE		51
#define MF_HWMON_PWM_MAX	255

/* Factory BIOS Baseline (Restored on module unload) */
static const u8 mf_bios_baseline_bases[7]  = { 0, 16, 18, 21, 28, 34, 36 };
static const u8 mf_bios_baseline_slopes[7] = { 0, 10, 33, 58, 60, 16, 200 };

static const char * const mf_temp_labels[] = {
	"CPU EC",
	"Motherboard",
	"VRM",
};

static const char * const mf_fan_labels[] = {
	"CPU Fan",
	"System Fan",
};

/*
 * Super I/O entry and exit helpers.
 */
static void mf_sio_enter(void)
{
	outb(0x87, MF_SIO_PORT_DEFAULT);
	outb(0x01, MF_SIO_PORT_DEFAULT);
	outb(0x55, MF_SIO_PORT_DEFAULT);
	outb(0x55, MF_SIO_PORT_DEFAULT);
}

static void mf_sio_exit(void)
{
	outb(0x02, MF_SIO_PORT_DEFAULT);
	outb(0x02, MF_SIO_DATA_DEFAULT);
}

/*
 * Low-level Super I/O SMFI indirect SRAM read for ITE IT5571.
 * Accesses on-chip peripheral registers and XRAM.
 */
static void mf_sio_sram_read(u16 addr, u8 *val)
{
	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x11, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	outb((addr >> 8) & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x10, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	outb(addr & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x12, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	*val = inb(MF_SIO_DATA_DEFAULT);
}

/*
 * Low-level Super I/O SMFI indirect SRAM write for ITE IT5571.
 */
static void mf_sio_sram_write(u16 addr, u8 val)
{
	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x11, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	outb((addr >> 8) & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x10, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	outb(addr & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(0x2E, MF_SIO_PORT_DEFAULT);
	outb(0x12, MF_SIO_DATA_DEFAULT);
	outb(0x2F, MF_SIO_PORT_DEFAULT);
	outb(val, MF_SIO_DATA_DEFAULT);
}

/*
 * Writes a 7-band hardware fan curve (bases and slopes) to EC XRAM.
 * Leaves critical safety protection row (>= 94C at 0x0325) untouched.
 */
static int mf_write_curve_table(struct minisforum_data *data, const u8 *bases, const u8 *slopes)
{
	int i;

	mutex_lock(&data->lock);
	mf_sio_enter();

	for (i = 0; i < 7; i++)
		mf_sio_sram_write(MF_REG_CPU_SLOPE_BASE + i, slopes[i]);

	for (i = 0; i < 7; i++)
		mf_sio_sram_write(MF_REG_CPU_BASE_START + (i * 3), bases[i]);

	mf_sio_exit();
	mutex_unlock(&data->lock);

	return 0;
}

/*
 * Reads the 16-bit tachometer counter and calculates live RPM.
 * Uses a 3-byte sample (low/high/low) to verify counter rollover.
 */
static int mf_read_fan_rpm(struct minisforum_data *data, u16 addr_lo, u16 addr_hi)
{
	u8 lo1 = 0, hi = 0, lo2 = 0;
	u32 counter;
	int attempt;

	mutex_lock(&data->lock);
	mf_sio_enter();

	for (attempt = 0; attempt < 3; attempt++) {
		mf_sio_sram_read(addr_lo, &lo1);
		mf_sio_sram_read(addr_hi, &hi);
		mf_sio_sram_read(addr_lo, &lo2);
		if (lo1 == lo2)
			break;
		udelay(10);
	}

	mf_sio_exit();
	mutex_unlock(&data->lock);

	counter = lo1 | ((u32)hi << 8);
	if (counter == 0)
		return 0;

	return MF_TACH_FREQ_HZ / counter;
}

/*
 * Applies a fan speed target code (0..51) to the 7 temperature bands.
 * Leaves the critical hardware thermal protection band (>= 94C) untouched.
 */
static int mf_apply_fan_code(struct minisforum_data *data, u8 code)
{
	int i;

	code = clamp_val(code, 0, MF_MAX_FAN_CODE);

	mutex_lock(&data->lock);
	mf_sio_enter();

	/* Set all 7 slopes to 0 for a fixed target */
	for (i = 0; i < 7; i++)
		mf_sio_sram_write(MF_REG_CPU_SLOPE_BASE + i, 0);

	/* Set all 7 base targets to requested code */
	for (i = 0; i < 7; i++)
		mf_sio_sram_write(MF_REG_CPU_BASE_START + (i * 3), code);

	mf_sio_exit();

	data->fan_target_code = code;
	mutex_unlock(&data->lock);

	return 0;
}

/*
 * Dynamic Governor Worker:
 * Evaluates composite system temperature (CPU die + VRM + Board)
 * with low-pass exponential moving average filtering, and applies
 * stepped rate-limited EC writes with cooldown hysteresis.
 */
static void mf_fan_worker(struct work_struct *work)
{
	struct minisforum_data *data = container_of(work, struct minisforum_data, fan_work.work);
	u8 t_cpu = 0, t_vrm = 0, t_board = 0;
	int comp_temp, eff_temp;
	u8 desired_code, current_code, next_code;
	int ret;

	/* Read composite temperatures from EC */
	ret = minisforum_ec_read(MF_EC_REG_TEMP_CPU, &t_cpu);
	if (ret)
		goto reschedule;
	ret = minisforum_ec_read(MF_EC_REG_TEMP_VRM, &t_vrm);
	if (ret)
		t_vrm = t_cpu;
	ret = minisforum_ec_read(MF_EC_REG_TEMP_BOARD, &t_board);
	if (ret)
		t_board = t_cpu;

	/*
	 * System Composite Weighted Temperature:
	 * 60% CPU/iGPU die + 20% VRM + 20% Motherboard.
	 * Accounts for both compute load and power delivery heat.
	 */
	comp_temp = ((int)t_cpu * 3 + (int)t_vrm + (int)t_board) / 5;

	/* Exponential Moving Average (EMA) low-pass filter to smooth transient spikes */
	if (data->temp_ema == 0)
		data->temp_ema = comp_temp * 1000;
	else
		data->temp_ema = (data->temp_ema * 3 + comp_temp * 1000) / 4;

	eff_temp = data->temp_ema / 1000;

	/* If manual override is active, do not touch EC fan target */
	if (data->fan_manual)
		goto reschedule;

	/*
	 * Determine ideal fan target code based on active profile:
	 * Code 6 (~550-600 RPM) is the rock-solid inaudible idle floor.
	 */
	switch (data->current_profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		if (eff_temp <= 48)
			desired_code = 6;  /* ~550-600 RPM silent idle */
		else if (eff_temp <= 56)
			desired_code = 8;  /* ~800 RPM */
		else if (eff_temp <= 66)
			desired_code = 12; /* ~1200 RPM */
		else if (eff_temp <= 78)
			desired_code = 18; /* ~1800 RPM */
		else
			desired_code = 28; /* ~2800 RPM */
		break;

	case PLATFORM_PROFILE_PERFORMANCE:
		if (eff_temp <= 42)
			desired_code = 10; /* ~1000 RPM */
		else if (eff_temp <= 50)
			desired_code = 16; /* ~1600 RPM */
		else if (eff_temp <= 62)
			desired_code = 24; /* ~2400 RPM */
		else if (eff_temp <= 75)
			desired_code = 34; /* ~3400 RPM */
		else
			desired_code = 48; /* ~4800 RPM */
		break;

	case PLATFORM_PROFILE_BALANCED:
	default:
		if (eff_temp <= 46)
			desired_code = 6;  /* ~550-600 RPM silent idle */
		else if (eff_temp <= 53)
			desired_code = 8;  /* ~800 RPM */
		else if (eff_temp <= 62)
			desired_code = 13; /* ~1300 RPM */
		else if (eff_temp <= 72)
			desired_code = 19; /* ~1900 RPM */
		else if (eff_temp <= 82)
			desired_code = 28; /* ~2800 RPM */
		else if (eff_temp <= 90)
			desired_code = 38; /* ~3800 RPM */
		else
			desired_code = 48; /* ~4800 RPM */
		break;
	}

	data->fan_desired_code = desired_code;
	current_code = data->fan_target_code;
	next_code = current_code;

	/*
	 * Stepped Rate Limiting & Cooldown Hysteresis:
	 * - Heating up: step up smoothly (+1 per second, or +2 if large thermal surge)
	 * - Cooling down: hold for 4 seconds hysteresis, then step down -1 per 2 seconds
	 */
	if (desired_code > current_code) {
		data->cooldown_timer = 0;
		if (desired_code - current_code >= 6)
			next_code = current_code + 2;
		else
			next_code = current_code + 1;
	} else if (desired_code < current_code) {
		data->cooldown_timer++;
		if (data->cooldown_timer >= 4) {
			next_code = current_code - 1;
			data->cooldown_timer = 2; /* Step down 1 code every 2 ticks */
		}
	} else {
		data->cooldown_timer = 0;
	}

	if (next_code != current_code) {
		mf_apply_fan_code(data, next_code);
	}

reschedule:
	queue_delayed_work(system_dfl_wq, &data->fan_work, msecs_to_jiffies(1000));
}

int minisforum_hwmon_apply_profile_curve(struct minisforum_data *data, enum platform_profile_option profile)
{
	data->cooldown_timer = 0;
	/* Trigger immediate re-evaluation on profile switch */
	mod_delayed_work(system_dfl_wq, &data->fan_work, 0);
	return 0;
}

static int mf_set_fan_manual(struct minisforum_data *data, bool manual)
{
	if (manual) {
		u8 code = data->fan_target_code;
		if (code == 0)
			code = 6;
		data->fan_manual = true;
		return mf_apply_fan_code(data, code);
	} else {
		data->fan_manual = false;
		data->cooldown_timer = 0;
		mod_delayed_work(system_dfl_wq, &data->fan_work, 0);
		dev_info(&data->pdev->dev, "CPU Fan restored to autonomous dynamic governor\n");
		return 0;
	}
}

static int mf_set_fan_pwm(struct minisforum_data *data, long pwm)
{
	u8 code = clamp_val(DIV_ROUND_CLOSEST(pwm * MF_MAX_FAN_CODE, MF_HWMON_PWM_MAX),
			    0, MF_MAX_FAN_CODE);
	data->fan_manual = true;
	return mf_apply_fan_code(data, code);
}

static umode_t mf_hwmon_is_visible(const void *drvdata,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static int mf_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	u8 reg, raw = 0;
	int ret;

	if (!data)
		return -ENODEV;

	switch (type) {
	case hwmon_temp:
		switch (channel) {
		case 0:
			reg = MF_EC_REG_TEMP_CPU;
			break;
		case 1:
			reg = MF_EC_REG_TEMP_BOARD;
			break;
		case 2:
			reg = MF_EC_REG_TEMP_VRM;
			break;
		default:
			return -EOPNOTSUPP;
		}

		ret = minisforum_ec_read(reg, &raw);
		if (ret)
			return ret;

		/* Standard hwmon temperature in millidegrees Celsius */
		*val = (long)raw * 1000;
		return 0;

	case hwmon_fan:
		switch (channel) {
		case 0:
			*val = mf_read_fan_rpm(data, MF_REG_TACH_CPU_LO, MF_REG_TACH_CPU_HI);
			return 0;
		case 1:
			*val = mf_read_fan_rpm(data, MF_REG_TACH_SYS_LO, MF_REG_TACH_SYS_HI);
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = clamp_val(DIV_ROUND_CLOSEST((long)data->fan_target_code * MF_HWMON_PWM_MAX,
							   MF_MAX_FAN_CODE),
					 0, MF_HWMON_PWM_MAX);
			return 0;
		case hwmon_pwm_enable:
			*val = data->fan_manual ? 1 : 2;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static int mf_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (!data)
		return -ENODEV;

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			val = clamp_val(val, 0, MF_HWMON_PWM_MAX);
			return mf_set_fan_pwm(data, val);
		case hwmon_pwm_enable:
			if (val == 1)
				return mf_set_fan_manual(data, true);
			else if (val == 2)
				return mf_set_fan_manual(data, false);
			return -EINVAL;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int mf_hwmon_read_string(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (channel >= 0 && channel < ARRAY_SIZE(mf_temp_labels)) {
			*str = mf_temp_labels[channel];
			return 0;
		}
		break;
	case hwmon_fan:
		if (channel >= 0 && channel < ARRAY_SIZE(mf_fan_labels)) {
			*str = mf_fan_labels[channel];
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops mf_hwmon_ops = {
	.is_visible = mf_hwmon_is_visible,
	.read = mf_hwmon_read,
	.read_string = mf_hwmon_read_string,
	.write = mf_hwmon_write,
};

static const struct hwmon_channel_info * const mf_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info mf_chip_info = {
	.ops = &mf_hwmon_ops,
	.info = mf_hwmon_info,
};

int minisforum_hwmon_init(struct minisforum_data *data)
{
	/* Copy factory BIOS baseline for safe restoration on driver exit */
	memcpy(data->fan_baseline_bases, mf_bios_baseline_bases, sizeof(mf_bios_baseline_bases));
	memcpy(data->fan_baseline_slopes, mf_bios_baseline_slopes, sizeof(mf_bios_baseline_slopes));

	/* Default to automatic mode with inaudible ~550-600 RPM idle floor */
	data->fan_manual = false;
	data->fan_target_code = 6;
	data->fan_desired_code = 6;
	data->temp_ema = 0;
	data->cooldown_timer = 0;

	/* Apply initial code 6 floor */
	mf_apply_fan_code(data, 6);

	/* Initialize and start the dynamic governor workqueue */
	INIT_DELAYED_WORK(&data->fan_work, mf_fan_worker);
	queue_delayed_work(system_dfl_wq, &data->fan_work, msecs_to_jiffies(1000));

	data->hwmon_dev = hwmon_device_register_with_info(&data->pdev->dev,
							  "minisforum",
							  data,
							  &mf_chip_info,
							  NULL);
	if (IS_ERR(data->hwmon_dev)) {
		dev_err(&data->pdev->dev, "Failed to register hwmon device: %ld\n",
			PTR_ERR(data->hwmon_dev));
		cancel_delayed_work_sync(&data->fan_work);
		return PTR_ERR(data->hwmon_dev);
	}

	dev_info(&data->pdev->dev, "HWMON sensors and Dynamic Thermal Governor registered\n");
	return 0;
}

void minisforum_hwmon_exit(struct minisforum_data *data)
{
	/* Stop dynamic governor workqueue */
	cancel_delayed_work_sync(&data->fan_work);

	/* Restore factory BIOS baseline curve to ensure safety after unload */
	mf_write_curve_table(data, data->fan_baseline_bases, data->fan_baseline_slopes);

	if (data->hwmon_dev) {
		hwmon_device_unregister(data->hwmon_dev);
		data->hwmon_dev = NULL;
	}
}
