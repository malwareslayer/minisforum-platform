// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Platform Driver Core
 *
 * Platform device registration, DMI table matching, and module lifecycle.
 */

#include "minisforum.h"

/* Board configuration profiles */
static const struct minisforum_fan_desc f7bsc_fans[] = {
	{
		.label = "CPU Fan",
		.temp_reg = MF_EC_REG_TEMP_CPU,
		.profile_reg = MF_EC_REG_CPU_PROFILE,
	},
	{
		.label = "System Fan",
		.temp_reg = MF_EC_REG_TEMP_BOARD,
		.profile_reg = 0,
	},
};

static const struct minisforum_board_config board_um790_pro = {
	.board_name = "F7BSC",
	.description = "Minisforum UM790 Pro (Venus series)",
	.chip_id = MF_CHIP_ID_IT5571,
	.sio_port = MF_SIO_PORT_DEFAULT,
	.num_fans = 2,
	.fans = f7bsc_fans,
};

static const struct minisforum_board_config board_um780_xtx = {
	.board_name = "F7BSD",
	.description = "Minisforum UM780 XTX",
	.chip_id = MF_CHIP_ID_IT5571,
	.sio_port = MF_SIO_PORT_DEFAULT,
	.num_fans = 2,
	.fans = f7bsc_fans,
};

static const struct minisforum_board_config board_um880_pro = {
	.board_name = "F7BSI",
	.description = "Minisforum EliteMini UM880 / UM890 Pro",
	.chip_id = MF_CHIP_ID_IT5571,
	.sio_port = MF_SIO_PORT_DEFAULT,
	.num_fans = 2,
	.fans = f7bsc_fans,
};

static const struct minisforum_board_config board_generic = {
	.board_name = "Generic",
	.description = "Generic Minisforum UM-Series Mini-PC",
	.chip_id = MF_CHIP_ID_IT5571,
	.sio_port = MF_SIO_PORT_DEFAULT,
	.num_fans = 1,
	.fans = f7bsc_fans,
};

/* DMI Matching Table for Auto-Detection */
static const struct dmi_system_id mf_dmi_table[] = {
	{
		.ident = "Minisforum UM790 Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "F7BSC"),
		},
		.driver_data = (void *)&board_um790_pro,
	},
	{
		.ident = "Minisforum UM780 XTX",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "F7BSD"),
		},
		.driver_data = (void *)&board_um780_xtx,
	},
	{
		.ident = "Minisforum UM880 Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "F7BSI"),
		},
		.driver_data = (void *)&board_um880_pro,
	},
	{
		.ident = "Minisforum UM890 Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "F7BSW"),
		},
		.driver_data = (void *)&board_um880_pro,
	},
	{ } /* Sentinel */
};
MODULE_DEVICE_TABLE(dmi, mf_dmi_table);

static struct minisforum_data *mf_data;

/* Safe ACPI EC Read Helper */
int minisforum_ec_read(u8 addr, u8 *val)
{
	return ec_read(addr, val);
}

/* Safe ACPI EC Write Helper */
int minisforum_ec_write(u8 addr, u8 val)
{
	return ec_write(addr, val);
}

static int minisforum_pm_notify(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct minisforum_data *mf = container_of(nb, struct minisforum_data, pm_nb);

	switch (action) {
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		/*
		 * The EC resets its profile register and fan curve across
		 * suspend, so both have to be re-applied on resume.
		 */
		minisforum_profile_set(mf, mf->current_profile);
		minisforum_hwmon_resume(mf);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static int __init minisforum_init(void)
{
	const struct dmi_system_id *dmi_match;
	const struct minisforum_board_config *board_config = NULL;
	u8 ec_ver_maj = 0, ec_ver_min = 0;
	int ret;

	pr_info(DRV_NAME ": Initializing Minisforum Platform Driver v%s\n", DRV_VERSION);

	/* 1. Check DMI Table */
	dmi_match = dmi_first_match(mf_dmi_table);
	if (dmi_match) {
		board_config = dmi_match->driver_data;
		pr_info(DRV_NAME ": Detected matched hardware: %s\n", board_config->description);
	} else {
		/* Fallback probe: Check if Minisforum EC is present */
		if (minisforum_ec_read(MF_EC_REG_VER_MAJ, &ec_ver_maj) == 0 &&
		    minisforum_ec_read(MF_EC_REG_VER_MIN, &ec_ver_min) == 0 &&
		    ec_ver_maj > 0) {
			board_config = &board_generic;
			pr_info(DRV_NAME ": Unlisted board detected with active EC v%u.%u, using generic profile\n",
				ec_ver_maj, ec_ver_min);
		} else {
			pr_warn(DRV_NAME ": No compatible Minisforum hardware detected.\n");
			return -ENODEV;
		}
	}

	/* 2. Allocate driver data */
	mf_data = kzalloc(sizeof(*mf_data), GFP_KERNEL);
	if (!mf_data)
		return -ENOMEM;

	mutex_init(&mf_data->lock);
	mf_data->board = board_config;

	/* 3. Register Platform Device */
	mf_data->pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(mf_data->pdev)) {
		ret = PTR_ERR(mf_data->pdev);
		kfree(mf_data);
		return ret;
	}

	platform_set_drvdata(mf_data->pdev, mf_data);

	/* 4. Initialize Platform Profile Subsystem */
	ret = minisforum_profile_init(mf_data);
	if (ret)
		pr_warn(DRV_NAME ": Failed to initialize platform_profile (err %d)\n", ret);

	/* 5. Initialize HWMON Subsystem */
	ret = minisforum_hwmon_init(mf_data);
	if (ret)
		pr_warn(DRV_NAME ": Failed to initialize hwmon (err %d)\n", ret);

	/* 6. Register PM Suspend/Resume Notifier */
	mf_data->pm_nb.notifier_call = minisforum_pm_notify;
	ret = register_pm_notifier(&mf_data->pm_nb);
	if (ret)
		pr_warn(DRV_NAME ": Failed to register PM notifier (err %d)\n", ret);

	pr_info(DRV_NAME ": Minisforum Platform Driver loaded successfully\n");
	return 0;
}

static void __exit minisforum_exit(void)
{
	if (!mf_data)
		return;

	unregister_pm_notifier(&mf_data->pm_nb);
	minisforum_hwmon_exit(mf_data);
	minisforum_profile_exit(mf_data);

	if (mf_data->pdev)
		platform_device_unregister(mf_data->pdev);

	mutex_destroy(&mf_data->lock);
	kfree(mf_data);
	mf_data = NULL;

	pr_info(DRV_NAME ": Minisforum Platform Driver unloaded\n");
}

module_init(minisforum_init);
module_exit(minisforum_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dwi Mulia Mokoginta <dwi-mulia-mokoginta@protonmail.com>");
MODULE_DESCRIPTION("Minisforum Linux Platform Driver");
MODULE_VERSION(DRV_VERSION);
