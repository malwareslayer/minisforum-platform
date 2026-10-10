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
#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/pm_qos.h>
#include <acpi/cppc_acpi.h>

#define DRV_NAME	"minisforum_platform"
#define DRV_VERSION	"0.1.0"

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
	struct mutex lock;
	bool fan_manual;
	u8 fan_target_code;
	u8 fan_desired_code;
	u8 fan_baseline_bases[7];
	u8 fan_baseline_slopes[7];
	struct delayed_work fan_work;
	int temp_ema;
	int cooldown_timer;
	struct freq_qos_request *qos_min_req;
	struct freq_qos_request *qos_max_req;
	bool qos_initialized;
	bool sync_in_progress;
	bool has_amd_pstate;
};

/* Subsystem Interface Prototypes */
int minisforum_profile_init(struct minisforum_data *data);
void minisforum_profile_exit(struct minisforum_data *data);
void minisforum_profile_sync_cpufreq(struct minisforum_data *data, enum platform_profile_option profile);
void minisforum_profile_check_governor(struct minisforum_data *data);

int minisforum_hwmon_init(struct minisforum_data *data);
void minisforum_hwmon_exit(struct minisforum_data *data);
int minisforum_hwmon_apply_profile_curve(struct minisforum_data *data, enum platform_profile_option profile);

/* Safe EC helpers */
int minisforum_ec_read(u8 addr, u8 *val);
int minisforum_ec_write(u8 addr, u8 val);

#endif /* _MINISFORUM_PLATFORM_H_ */
