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

/* Tachometer counter frequency of the IT5571 family. */
#define MF_TACH_FREQ_HZ		2156250

/* ITE Super I/O config-space indirect access registers */
#define MF_SIO_IDX_SMFI_REG	0x11	/* SMFI address high */
#define MF_SIO_IDX_SMFI_ADDR	0x10	/* SMFI address low */
#define MF_SIO_IDX_SMFI_DATA	0x12	/* SMFI data */

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
 * @port is shared with the generic it87 driver, which claims the same two
 * ports, so the window must be reserved before it is touched and released
 * afterwards. Returns -EBUSY when another driver holds it; callers propagate
 * that instead of writing to an unowned port.
 */
static int mf_sio_lock(u16 port)
{
	if (!request_muxed_region(port, MF_SIO_WINDOW_SIZE, DRV_NAME))
		return -EBUSY;

	/* ITE config-space unlock sequence: 0x87, 0x01, 0x55, 0xAA-equivalent. */
	outb(0x87, port);
	outb(0x01, port);
	outb(0x55, port);
	outb(0x55, port);

	return 0;
}

static void mf_sio_unlock(u16 port)
{
	outb(0x02, port);
	outb(0x02, port + 1);

	release_region(port, MF_SIO_WINDOW_SIZE);
}

/*
 * Indirect SRAM read through the SMFI interface. The Super I/O window must
 * already be claimed by mf_sio_lock().
 */
static void mf_sio_sram_read(u16 port, u16 addr, u8 *val)
{
	u16 data_port = port + 1;

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_REG, data_port);
	outb(data_port, port);
	outb((addr >> 8) & 0xFF, data_port);

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_ADDR, data_port);
	outb(data_port, port);
	outb(addr & 0xFF, data_port);

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_DATA, data_port);
	outb(data_port, port);
	*val = inb(data_port);
}

/*
 * Indirect SRAM write through the SMFI interface. The Super I/O window must
 * already be claimed by mf_sio_lock().
 */
static void mf_sio_sram_write(u16 port, u16 addr, u8 val)
{
	u16 data_port = port + 1;

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_REG, data_port);
	outb(data_port, port);
	outb((addr >> 8) & 0xFF, data_port);

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_ADDR, data_port);
	outb(data_port, port);
	outb(addr & 0xFF, data_port);

	outb(port, port);
	outb(MF_SIO_IDX_SMFI_DATA, data_port);
	outb(data_port, port);
	outb(val, data_port);
}

/*
 * Writes a 7-band hardware fan curve (bases and slopes) to EC XRAM.
 * Leaves the critical safety protection row (>= 94C) untouched.
 */
static int mf_write_curve_table(struct minisforum_data *data,
				const u8 *bases, const u8 *slopes)
{
	const struct minisforum_board_config *cfg = data->board;
	int ret, i;

	mutex_lock(&data->lock);

	ret = mf_sio_lock(cfg->sio_port);
	if (ret)
		goto out_unlock;

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(cfg->sio_port, cfg->xram_curve_slope + i,
				  slopes[i]);

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(cfg->sio_port,
				  cfg->xram_curve_base + (i * 3), bases[i]);

	mf_sio_unlock(cfg->sio_port);

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
	const struct minisforum_board_config *cfg = data->board;
	u8 bases[MF_FAN_CURVE_BANDS];
	u8 slopes[MF_FAN_CURVE_BANDS];
	bool sane = true;
	int ret, i;

	mutex_lock(&data->lock);
	ret = mf_sio_lock(cfg->sio_port);
	if (!ret) {
		for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
			mf_sio_sram_read(cfg->sio_port,
					 cfg->xram_curve_slope + i, &slopes[i]);

		for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
			mf_sio_sram_read(cfg->sio_port,
					 cfg->xram_curve_base + (i * 3), &bases[i]);

		mf_sio_unlock(cfg->sio_port);
	}
	mutex_unlock(&data->lock);

	/* A band target above the EC's duty range means the read went wrong. */
	if (!ret) {
		for (i = 0; i < MF_FAN_CURVE_BANDS; i++) {
			if (bases[i] > cfg->max_fan_code)
				sane = false;
		}
	}

	if (!ret && sane) {
		memcpy(data->fan_baseline_bases, bases, sizeof(bases));
		memcpy(data->fan_baseline_slopes, slopes, sizeof(slopes));
		return;
	}

	memcpy(data->fan_baseline_bases, cfg->fallback_bases,
	       sizeof(data->fan_baseline_bases));
	memcpy(data->fan_baseline_slopes, cfg->fallback_slopes,
	       sizeof(data->fan_baseline_slopes));
	dev_warn(data->dev, "EC fan curve unreadable, using board defaults\n");
}

/*
 * Reads the 16-bit tachometer counter and calculates live RPM. Samples the
 * low byte twice so a counter rollover between reads is detected and retried.
 */
static int mf_read_fan_rpm(struct minisforum_data *data, u16 addr_lo, long *rpm)
{
	u8 lo1 = 0, hi = 0, lo2 = 0;
	u32 counter;
	int ret, attempt;

	mutex_lock(&data->lock);

	ret = mf_sio_lock(data->board->sio_port);
	if (ret)
		goto out_unlock;

	for (attempt = 0; attempt < 3; attempt++) {
		mf_sio_sram_read(data->board->sio_port, addr_lo, &lo1);
		mf_sio_sram_read(data->board->sio_port, addr_lo + 1, &hi);
		mf_sio_sram_read(data->board->sio_port, addr_lo, &lo2);
		if (lo1 == lo2)
			break;
		usleep_range(10, 25);
	}

	mf_sio_unlock(data->board->sio_port);

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
	const struct minisforum_board_config *cfg = data->board;
	int ret, i;

	code = clamp_val(code, 0, cfg->max_fan_code);

	mutex_lock(&data->lock);

	ret = mf_sio_lock(cfg->sio_port);
	if (ret)
		goto out_unlock;

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(cfg->sio_port, cfg->xram_curve_slope + i, 0);

	for (i = 0; i < MF_FAN_CURVE_BANDS; i++)
		mf_sio_sram_write(cfg->sio_port,
				  cfg->xram_curve_base + (i * 3), code);

	mf_sio_unlock(cfg->sio_port);

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
	const struct minisforum_board_config *cfg = data->board;

	switch (val) {
	case MF_PWM_ENABLE_FULL:
		/* No fan speed control: run the fan at full speed. */
		data->pwm_enable = MF_PWM_ENABLE_FULL;
		return mf_apply_fan_code(data, cfg->max_fan_code);

	case MF_PWM_ENABLE_MANUAL:
		/* Userspace owns pwm1. Start from the duty in effect: no jump. */
		if (data->pwm_enable != MF_PWM_ENABLE_MANUAL) {
			data->manual_code = data->fan_code;
			if (data->manual_code < cfg->min_fan_code)
				data->manual_code = cfg->min_fan_code;
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
	const struct minisforum_board_config *cfg = data->board;
	/*
	 * Clamp to the lowest duty the EC drives the fan without stalling so
	 * that a stray "echo 0 > pwm1" cannot stop the CPU fan.
	 */
	u8 code = clamp_val(DIV_ROUND_CLOSEST(pwm * cfg->max_fan_code,
					      MF_HWMON_PWM_MAX),
			    cfg->min_fan_code, cfg->max_fan_code);

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
	const struct minisforum_board_config *cfg = data->board;
	u8 reg, raw = 0;
	int ret;

	switch (type) {
	case hwmon_temp:
		switch (channel) {
		case 0:
			reg = cfg->ec_temp_cpu;
			break;
		case 1:
			reg = cfg->ec_temp_board;
			break;
		case 2:
			reg = cfg->ec_temp_vrm;
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
			return mf_read_fan_rpm(data, cfg->xram_tach_cpu, val);
		case 1:
			return mf_read_fan_rpm(data, cfg->xram_tach_sys, val);
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = DIV_ROUND_CLOSEST((long)data->fan_code *
						 MF_HWMON_PWM_MAX,
						 cfg->max_fan_code);
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
		mf_apply_fan_code(data, data->board->max_fan_code);
		break;
	case MF_PWM_ENABLE_MANUAL:
		mf_apply_fan_code(data, data->manual_code);
		break;
	default:
		break;
	}
}
