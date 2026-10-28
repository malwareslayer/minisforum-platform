// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Platform Driver Core
 *
 * DMI matching and platform driver lifecycle.
 */

#include "minisforum.h"

static const struct minisforum_board_config board_um790_pro = {
	.board_name = "F7BSC",
	.description = "Minisforum UM790 Pro (Venus series)",
	.chip_id = MF_CHIP_ID_IT5571,
	.num_fans = 2,
};

static const struct minisforum_board_config board_um780_xtx = {
	.board_name = "F7BSD",
	.description = "Minisforum UM780 XTX",
	.chip_id = MF_CHIP_ID_IT5571,
	.num_fans = 2,
};

static const struct minisforum_board_config board_um880_pro = {
	.board_name = "F7BSI",
	.description = "Minisforum EliteMini UM880 Pro",
	.chip_id = MF_CHIP_ID_IT5571,
	.num_fans = 2,
};

static const struct minisforum_board_config board_um890_pro = {
	.board_name = "F7BSW",
	.description = "Minisforum UM890 Pro",
	.chip_id = MF_CHIP_ID_IT5571,
	.num_fans = 2,
};

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
		.driver_data = (void *)&board_um890_pro,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, mf_dmi_table);

static int minisforum_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct dmi_system_id *dmi_match;
	struct minisforum_data *data;
	int ret;

	/* Only bind to hardware whose register layout we actually know. */
	dmi_match = dmi_first_match(mf_dmi_table);
	if (!dmi_match)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->board = dmi_match->driver_data;
	platform_set_drvdata(pdev, data);

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	ret = minisforum_profile_init(dev, data);
	if (ret)
		return ret;

	ret = minisforum_hwmon_init(dev, data);
	if (ret)
		return ret;

	dev_info(dev, "%s\n", data->board->description);
	return 0;
}

static void minisforum_remove(struct platform_device *pdev)
{
	/* Never leave the fan stuck at a userspace duty. */
	minisforum_hwmon_remove(platform_get_drvdata(pdev));
}

static int __maybe_unused minisforum_resume(struct device *dev)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	/*
	 * The EC resets its profile register and fan curve across suspend,
	 * so both have to be re-applied on resume. SIMPLE_DEV_PM_OPS also
	 * covers the hibernation restore path.
	 */
	minisforum_profile_set(data, data->current_profile);
	minisforum_hwmon_resume(data);

	/*
	 * If the firmware refused the re-apply, adopt its value and tell
	 * userspace so tuned/ppd do not keep showing the old profile.
	 */
	minisforum_profile_sync(data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(minisforum_pm_ops, NULL, minisforum_resume);

static struct platform_driver minisforum_driver = {
	.probe = minisforum_probe,
	.remove = minisforum_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = &minisforum_pm_ops,
	},
};

static struct platform_device *minisforum_pdev;

static int __init minisforum_init(void)
{
	int ret;

	minisforum_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(minisforum_pdev))
		return PTR_ERR(minisforum_pdev);

	ret = platform_driver_register(&minisforum_driver);
	if (ret) {
		platform_device_unregister(minisforum_pdev);
		return ret;
	}

	return 0;
}

static void __exit minisforum_exit(void)
{
	platform_driver_unregister(&minisforum_driver);
	platform_device_unregister(minisforum_pdev);
}

module_init(minisforum_init);
module_exit(minisforum_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dwi Mulia Mokoginta <dwi-mulia-mokoginta@protonmail.com>");
MODULE_DESCRIPTION("Minisforum platform and hardware monitoring driver");
