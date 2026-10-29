/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minisforum Platform Driver
 *
 * Common definitions, board configurations, and subsystem interfaces.
 */

#ifndef _MINISFORUM_PLATFORM_H_
#define _MINISFORUM_PLATFORM_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/suspend.h>

#define DRV_NAME	"minisforum_platform"

/*
 * Standard hwmon pwmN_enable semantics (Documentation/hwmon/sysfs-interface.rst).
 * No thermal policy lives in this driver: MODE_AUTO hands the fan back to the
 * EC firmware's own curve, and custom curves belong in userspace.
 */
#define MF_PWM_ENABLE_FULL	0	/* No fan speed control: fan at full speed */
#define MF_PWM_ENABLE_MANUAL	1	/* Manual: userspace owns pwm1 */
#define MF_PWM_ENABLE_AUTO	2	/* Automatic: EC firmware thermal curve */

/* Standard hwmon PWM range, independent of the EC's own duty codes. */
#define MF_HWMON_PWM_MAX	255

/* Number of temperature bands in the EC fan lookup table. */
#define MF_FAN_CURVE_BANDS	7

/* Super I/O configuration window, shared with the generic it87 driver. */
#define MF_SIO_WINDOW_SIZE	2

#define MF_CHIP_ID_IT5571	0x5571
#define MF_CHIP_ID_IT5570	0x5570

/**
 * struct minisforum_board_config - EC layout of one board
 * @board_name: DMI board name this layout was written for
 * @description: human readable model name
 * @chip_id: expected Super I/O chip ID
 * @num_fans: number of tachometers wired on the board
 * @sio_port: Super I/O index port; the data port is the next address
 * @ec_temp_cpu: EC register holding the CPU/board-side temperature
 * @ec_temp_board: EC register holding the motherboard temperature
 * @ec_temp_vrm: EC register holding the VRM temperature
 * @ec_profile: EC register holding the CPU power profile selector
 * @profile_quiet: selector value for low-power
 * @profile_balanced: selector value for balanced
 * @profile_performance: selector value for performance
 * @xram_curve_base: XRAM address of the first fan curve band
 * @xram_curve_slope: XRAM address of the first fan curve slope
 * @xram_tach_cpu: XRAM address of the CPU tachometer counter (low byte)
 * @xram_tach_sys: XRAM address of the system tachometer counter (low byte)
 * @max_fan_code: highest duty code the EC accepts
 * @min_fan_code: lowest duty code that still turns the fan
 * @fallback_bases: factory curve band targets, used if the EC cannot be read
 * @fallback_slopes: factory curve slopes, used if the EC cannot be read
 *
 * The EC register layout is firmware specific and is not covered by a public
 * datasheet, so it is described per board instead of being assumed globally.
 * A board whose firmware places these registers elsewhere needs its own entry
 * here, not only a DMI match.
 */
struct minisforum_board_config {
	const char *board_name;
	const char *description;
	u16 chip_id;
	int num_fans;
	u16 sio_port;

	u8 ec_temp_cpu;
	u8 ec_temp_board;
	u8 ec_temp_vrm;
	u8 ec_profile;
	u8 profile_quiet;
	u8 profile_balanced;
	u8 profile_performance;

	u16 xram_curve_base;
	u16 xram_curve_slope;
	u16 xram_tach_cpu;
	u16 xram_tach_sys;

	u8 max_fan_code;
	u8 min_fan_code;

	const u8 *fallback_bases;
	const u8 *fallback_slopes;
};

struct minisforum_data {
	struct device *dev;
	struct device *pp_dev;
	const struct minisforum_board_config *board;
	enum platform_profile_option current_profile;
	struct mutex lock; /* Serializes EC access and protects the fields below */
	u8 pwm_enable;     /* 0 = full, 1 = manual, 2 = EC firmware curve */
	u8 fan_code;       /* Duty currently programmed into the EC */
	u8 manual_code;    /* Last duty requested by userspace */
	u8 fan_baseline_bases[MF_FAN_CURVE_BANDS];
	u8 fan_baseline_slopes[MF_FAN_CURVE_BANDS];
};

/* Subsystem Interface Prototypes */
int minisforum_profile_init(struct device *dev, struct minisforum_data *data);
int minisforum_profile_set(struct minisforum_data *data,
			   enum platform_profile_option profile);
void minisforum_profile_sync(struct minisforum_data *data);

int minisforum_hwmon_init(struct device *dev, struct minisforum_data *data);
void minisforum_hwmon_remove(struct minisforum_data *data);
void minisforum_hwmon_resume(struct minisforum_data *data);

#endif /* _MINISFORUM_PLATFORM_H_ */
