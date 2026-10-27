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
#include <linux/init.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/suspend.h>

#define DRV_NAME	"minisforum_platform"
#define DRV_VERSION	"0.4.0"

/*
 * Standard hwmon pwmN_enable semantics (Documentation/hwmon/sysfs-interface.rst).
 * No thermal policy lives in this driver: MODE_AUTO hands the fan back to the
 * EC firmware's own curve, and custom curves belong in userspace.
 */
#define MF_PWM_ENABLE_FULL	0	/* No fan speed control: fan at full speed */
#define MF_PWM_ENABLE_MANUAL	1	/* Manual: userspace owns pwm1 */
#define MF_PWM_ENABLE_AUTO	2	/* Automatic: EC firmware thermal curve */

/* EC fan duty is a 0..51 code; hwmon exposes the standard 0..255 range. */
#define MF_MAX_FAN_CODE		51
#define MF_HWMON_PWM_MAX	255
/*
 * Safety floor for manual control. Code 5 (~450-500 RPM) is the lowest duty
 * the EC has been observed to drive the fan without stalling; below it the
 * tachometer reads 0 and the EC thermal protection has to step in, which on
 * this board means an abrupt fan ramp. Manual writes are clamped here so a
 * stray "echo 0 > pwm1" cannot stop the CPU fan.
 */
#define MF_MANUAL_MIN_CODE	5

/* Number of temperature bands in the EC fan lookup table. */
#define MF_FAN_CURVE_BANDS	7

/* EC Register Map (ACPI EC 0x62/0x66 address space) */
#define MF_EC_REG_VER_MAJ		0x02
#define MF_EC_REG_VER_MIN		0x03
#define MF_EC_REG_TEMP_VRM		0x04
#define MF_EC_REG_TEMP_BOARD		0x05
#define MF_EC_REG_TEMP_CPU		0x09
#define MF_EC_REG_FAN_TABLE_START	0x10
#define MF_EC_REG_FAN_TABLE_END		0x27
#define MF_EC_REG_CPU_PROFILE		0x2F
#define MF_EC_REG_SYS_POLICY_START	0x30
#define MF_EC_REG_SYS_POLICY_END	0x38

/* Firmware CPU Profile Selectors */
#define MF_EC_PROFILE_QUIET		0x00
#define MF_EC_PROFILE_BALANCED		0xB1
#define MF_EC_PROFILE_PERFORMANCE	0xB2

/* Super I/O Ports & IDs */
#define MF_SIO_PORT_DEFAULT		0x2E
#define MF_SIO_DATA_DEFAULT		0x2F
#define MF_CHIP_ID_IT5571		0x5571
#define MF_CHIP_ID_IT5570		0x5570

struct minisforum_fan_desc {
	const char *label;
	u8 temp_reg;
	u8 profile_reg;
};

struct minisforum_board_config {
	const char *board_name;
	const char *description;
	u16 chip_id;
	u16 sio_port;
	int num_fans;
	const struct minisforum_fan_desc *fans;
};

struct minisforum_data {
	struct platform_device *pdev;
	struct device *pp_dev;
	struct device *hwmon_dev;
	const struct minisforum_board_config *board;
	enum platform_profile_option current_profile;
	struct mutex lock; /* Protects EC communication and state transitions */
	u8 pwm_enable;     /* 0 = full, 1 = manual, 2 = EC firmware curve */
	u8 fan_code;       /* Duty currently programmed into the EC (0..51) */
	u8 manual_code;    /* Last duty requested by userspace (0..51) */
	u8 fan_baseline_bases[MF_FAN_CURVE_BANDS];
	u8 fan_baseline_slopes[MF_FAN_CURVE_BANDS];
	struct notifier_block pm_nb;
};

/* Subsystem Interface Prototypes */
int minisforum_profile_init(struct minisforum_data *data);
void minisforum_profile_exit(struct minisforum_data *data);
int minisforum_profile_set(struct minisforum_data *data,
			   enum platform_profile_option profile);

int minisforum_hwmon_init(struct minisforum_data *data);
void minisforum_hwmon_exit(struct minisforum_data *data);
void minisforum_hwmon_resume(struct minisforum_data *data);

/* Safe EC helpers */
int minisforum_ec_read(u8 addr, u8 *val);
int minisforum_ec_write(u8 addr, u8 val);

#endif /* _MINISFORUM_PLATFORM_H_ */
