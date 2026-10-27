// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Hardware Monitoring Driver (HWMON)
 *
 * Exposes the board temperature sensors, fan tachometers and standard PWM
 * fan control (pwm1 / pwm1_enable / pwm1_mode).
 *
 * This driver implements mechanism only and carries no thermal policy of its
 * own. pwm1_enable 2 hands the fan back to the EC firmware's autonomous
 * curve; a custom curve belongs in userspace (fancontrol, CoolerControl)
 * via pwm1_enable 1.
 */

#include <linux/io.h>
#include <linux/delay.h>
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

/* ITE Super I/O config-space indirect access registers */
#define MF_SIO_IDX_SMFI_REG	0x11	/* SMFI address high */
#define MF_SIO_IDX_SMFI_ADDR	0x10	/* SMFI address low */
#define MF_SIO_IDX_SMFI_DATA	0x12	/* SMFI data */

/*
 * Factory curve of this board, used only when the EC snapshot at probe does
 * not look sane. It is never written unless the fan is handed back to the
 * firmware or the module is unloaded.
 */
static const u8 mf_fallback_bases[MF_FAN_CURVE_BANDS]  = { 0, 16, 18, 21, 28, 34, 36 };
static const u8 mf_fallback_slopes[MF_FAN_CURVE_BANDS] = { 0, 10, 33, 58, 60, 16, 200 };

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
 * Claim the Super I/O configuration window and unlock it.
 *
 * MF_SIO_PORT_DEFAULT is shared with the generic it87 driver, which claims
 * the same two ports, so the window must be reserved before it is touched and
 * released afterwards. Returns -EBUSY when another driver holds it; callers
 * propagate that instead of writing to an unowned port.
 */
static int mf_sio_lock(void)
{
	if (!request_muxed_region(MF_SIO_PORT_DEFAULT, MF_SIO_WINDOW_SIZE,
				  DRV_NAME))
		return -EBUSY;

	/* ITE config-space unlock sequence: 0x87, 0x01, 0x55, 0xAA-equivalent. */
	outb(0x87, MF_SIO_PORT_DEFAULT);
	outb(0x01, MF_SIO_PORT_DEFAULT);
	outb(0x55, MF_SIO_PORT_DEFAULT);
	outb(0x55, MF_SIO_PORT_DEFAULT);

	return 0;
}

static void mf_sio_unlock(void)
{
	outb(0x02, MF_SIO_PORT_DEFAULT);
	outb(0x02, MF_SIO_DATA_DEFAULT);

	release_region(MF_SIO_PORT_DEFAULT, MF_SIO_WINDOW_SIZE);
}

/*
 * Indirect SRAM read through the SMFI interface. The Super I/O window must
 * already be claimed by mf_sio_lock().
 */
static void mf_sio_sram_read(u16 addr, u8 *val)
{
	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_REG, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb((addr >> 8) & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_ADDR, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(addr & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_DATA, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	*val = inb(MF_SIO_DATA_DEFAULT);
}

/*
 * Indirect SRAM write through the SMFI interface. The Super I/O window must
 * already be claimed by mf_sio_lock().
 */
static void mf_sio_sram_write(u16 addr, u8 val)
{
	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_REG, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb((addr >> 8) & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_ADDR, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(addr & 0xFF, MF_SIO_DATA_DEFAULT);

	outb(MF_SIO_PORT_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(MF_SIO_IDX_SMFI_DATA, MF_SIO_DATA_DEFAULT);
	outb(MF_SIO_DATA_DEFAULT, MF_SIO_PORT_DEFAULT);
	outb(val, MF_SIO_DATA_DEFAULT);
}

/*
 * Writes a 7-band hardware fan curve (bases and slopes) to EC XRAM.
 * Leaves the critical safety protection row (>= 94C at 0x0325) untouched.
 */
static int mf_write_curve_table(struct minisforum_data *data,
				const u8 *bases, const u8 *slopes)
{
	int ret, i;

	mutex_lock(&data->lock);

	ret = mf_sio_lock();
	if (ret)
		goto out_unlock;

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(MF_REG_CPU_SLOPE_BASE + i, slopes[i]);

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(MF_REG_CPU_BASE_START + (i * 3), bases[i]);

	mf_sio_unlock();

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * Snapshot the EC's own fan curve so it can be handed back untouched when
 * userspace releases the fan or the module is unloaded. Reading the live
 * table (instead of trusting a hardcoded one) keeps the firmware in charge
 * of what "automatic" actually means.
 */
static void mf_snapshot_curve(struct minisforum_data *data)
{
	u8 bases[MF_FAN_CURVE_BANDS];
	u8 slopes[MF_FAN_CURVE_BANDS];
	bool sane = true;
	int ret, i;

	mutex_lock(&data->lock);
	ret = mf_sio_lock();
	if (!ret) {
		for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
			mf_sio_sram_read(MF_REG_CPU_SLOPE_BASE + i,
					 &slopes[i]);

		for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
			mf_sio_sram_read(MF_REG_CPU_BASE_START + (i * 3),
					 &bases[i]);

		mf_sio_unlock();
	}
	mutex_unlock(&data->lock);

	/* A band target above the EC's duty range means the read went wrong. */
	if (!ret) {
		for (i = 0; i < MF_FAN_CURVE_BANDS; i++) {
			if (bases[i] > MF_MAX_FAN_CODE)
				sane = false;
		}
	}

	if (!ret && sane) {
		memcpy(data->fan_baseline_bases, bases, sizeof(bases));
		memcpy(data->fan_baseline_slopes, slopes, sizeof(slopes));
		return;
	}

	memcpy(data->fan_baseline_bases, mf_fallback_bases,
	       sizeof(mf_fallback_bases));
	memcpy(data->fan_baseline_slopes, mf_fallback_slopes,
	       sizeof(mf_fallback_slopes));
	dev_warn(data->dev, "EC fan curve unreadable, using factory defaults\n");
}

/*
 * Reads the 16-bit tachometer counter and calculates live RPM. Samples the
 * low byte twice so a counter rollover between reads is detected and retried.
 */
static int mf_read_fan_rpm(struct minisforum_data *data, u16 addr_lo,
			   u16 addr_hi, long *rpm)
{
	u8 lo1 = 0, hi = 0, lo2 = 0;
	u32 counter;
	int ret, attempt;

	mutex_lock(&data->lock);

	ret = mf_sio_lock();
	if (ret)
		goto out_unlock;

	for (attempt = 0; attempt < 3; attempt++) {
		mf_sio_sram_read(addr_lo, &lo1);
		mf_sio_sram_read(addr_hi, &hi);
		mf_sio_sram_read(addr_lo, &lo2);
		if (lo1 == lo2)
			break;
		usleep_range(10, 25);
	}

	mf_sio_unlock();

	counter = lo1 | ((u32)hi << 8);
	*rpm = counter ? MF_TACH_FREQ_HZ / counter : 0;

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * Programs a fixed duty code into every temperature band, which turns the
 * EC's lookup table into a constant target. The EC firmware then holds that
 * duty on its own: no periodic rewrite from the kernel is needed, so this
 * stays a mechanism rather than a policy.
 *
 * Leaves the critical hardware thermal protection band (>= 94C) untouched.
 */
static int mf_apply_fan_code(struct minisforum_data *data, u8 code)
{
	int ret, i;

	code = clamp_val(code, 0, MF_MAX_FAN_CODE);

	mutex_lock(&data->lock);

	ret = mf_sio_lock();
	if (ret)
		goto out_unlock;

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(MF_REG_CPU_SLOPE_BASE + i, 0);

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(MF_REG_CPU_BASE_START + (i * 3), code);

	mf_sio_unlock();

	data->fan_code = code;

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * Map the standard pwm1_enable values onto the EC fan registers.
 */
static int mf_set_pwm_enable(struct minisforum_data *data, long val)
{
	switch (val) {
	case MF_PWM_ENABLE_FULL:
		/* No fan speed control: run the fan at full speed. */
		data->pwm_enable = MF_PWM_ENABLE_FULL;
		return mf_apply_fan_code(data, MF_MAX_FAN_CODE);

	case MF_PWM_ENABLE_MANUAL:
		/* Userspace owns pwm1. Start from the duty in effect: no jump. */
		if (data->pwm_enable != MF_PWM_ENABLE_MANUAL) {
			data->manual_code = data->fan_code;
			if (data->manual_code < MF_MANUAL_MIN_CODE)
				data->manual_code = MF_MANUAL_MIN_CODE;
		}
		data->pwm_enable = MF_PWM_ENABLE_MANUAL;
		return mf_apply_fan_code(data, data->manual_code);

	case MF_PWM_ENABLE_AUTO:
		/* Hand the fan back to the EC firmware's own thermal curve. */
		data->pwm_enable = MF_PWM_ENABLE_AUTO;
		/*
		 * No single duty is programmed by the driver in this mode, so
		 * pwm1 must stop reporting the last manual value. Read
		 * fan1_input for the resulting speed.
		 */
		data->fan_code = 0;
		dev_dbg(data->dev, "CPU fan handed back to EC firmware curve\n");
		return mf_write_curve_table(data, data->fan_baseline_bases,
					    data->fan_baseline_slopes);

	default:
		return -EINVAL;
	}
}

static int mf_set_fan_pwm(struct minisforum_data *data, long pwm)
{
	/*
	 * Clamp to the lowest duty the EC drives the fan without stalling so
	 * that a stray "echo 0 > pwm1" cannot stop the CPU fan.
	 */
	u8 code = clamp_val(DIV_ROUND_CLOSEST(pwm * MF_MAX_FAN_CODE,
					      MF_HWMON_PWM_MAX),
			    MF_MANUAL_MIN_CODE, MF_MAX_FAN_CODE);

	/*
	 * Writing pwm1 is an explicit request to own the fan. Flip to manual
	 * instead of returning -EPERM so tools that write the duty before
	 * touching pwm1_enable (CoolerControl, plain scripts) work too.
	 */
	data->pwm_enable = MF_PWM_ENABLE_MANUAL;
	data->manual_code = code;

	return mf_apply_fan_code(data, code);
}

static umode_t mf_hwmon_is_visible(const void *drvdata,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	const struct minisforum_data *data = drvdata;

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
		/* Boards with a single tachometer must not expose fan2. */
		if (channel >= data->board->num_fans)
			return 0;

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
		case hwmon_pwm_mode:
			return 0444;
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

		ret = ec_read(reg, &raw);
		if (ret)
			return ret;

		/* Standard hwmon temperature in millidegrees Celsius */
		*val = (long)raw * 1000;
		return 0;

	case hwmon_fan:
		switch (channel) {
		case 0:
			return mf_read_fan_rpm(data, MF_REG_TACH_CPU_LO,
					       MF_REG_TACH_CPU_HI, val);
		case 1:
			return mf_read_fan_rpm(data, MF_REG_TACH_SYS_LO,
					       MF_REG_TACH_SYS_HI, val);
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = DIV_ROUND_CLOSEST((long)data->fan_code *
						 MF_HWMON_PWM_MAX,
						 MF_MAX_FAN_CODE);
			return 0;
		case hwmon_pwm_enable:
			*val = data->pwm_enable;
			return 0;
		case hwmon_pwm_mode:
			/* IT5571 drives the fan with a PWM signal, not DC. */
			*val = 1;
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

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			val = clamp_val(val, 0, MF_HWMON_PWM_MAX);
			return mf_set_fan_pwm(data, val);
		case hwmon_pwm_enable:
			return mf_set_pwm_enable(data, val);
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
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE |
			   HWMON_PWM_MODE),
	NULL
};

static const struct hwmon_chip_info mf_chip_info = {
	.ops = &mf_hwmon_ops,
	.info = mf_hwmon_info,
};

int minisforum_hwmon_init(struct device *dev, struct minisforum_data *data)
{
	struct device *hwmon;

	/*
	 * Remember the firmware's own curve, then leave the EC alone: the fan
	 * stays under firmware control until userspace explicitly asks for it.
	 */
	mf_snapshot_curve(data);

	data->pwm_enable = MF_PWM_ENABLE_AUTO;
	data->manual_code = 0;
	data->fan_code = 0;

	hwmon = devm_hwmon_device_register_with_info(dev, "minisforum", data,
						     &mf_chip_info, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(dev, PTR_ERR(hwmon),
				     "failed to register hwmon device\n");

	dev_info(dev, "hwmon sensors registered\n");
	return 0;
}

void minisforum_hwmon_remove(struct minisforum_data *data)
{
	int ret;

	/*
	 * Never leave the fan stuck at a userspace duty: hand it back to the
	 * firmware curve we snapshotted at probe.
	 */
	if (data->pwm_enable == MF_PWM_ENABLE_AUTO)
		return;

	ret = mf_write_curve_table(data, data->fan_baseline_bases,
				   data->fan_baseline_slopes);
	if (ret)
		dev_warn(data->dev, "failed to restore fan curve: %d\n", ret);
}

/*
 * The EC resets to its own curve across suspend, so a manual duty has to be
 * re-applied (an automatic one is already what the firmware restores).
 */
void minisforum_hwmon_resume(struct minisforum_data *data)
{
	switch (data->pwm_enable) {
	case MF_PWM_ENABLE_FULL:
		mf_apply_fan_code(data, MF_MAX_FAN_CODE);
		break;
	case MF_PWM_ENABLE_MANUAL:
		mf_apply_fan_code(data, data->manual_code);
		break;
	default:
		break;
	}
}
