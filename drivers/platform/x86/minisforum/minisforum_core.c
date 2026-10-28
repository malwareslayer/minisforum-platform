// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Platform Driver Core
 *
 * DMI matching and platform driver lifecycle.
 */

#include "minisforum.h"

/*
 * Factory fan curve these boards ship with. Only written when the live table
 * cannot be read at probe, so a failed snapshot still leaves a usable fan.
 */
static const u8 mf_um_fallback_bases[MF_FAN_CURVE_BANDS] = { 0, 16, 18, 21, 28, 34, 36 };
static const u8 mf_um_fallback_slopes[MF_FAN_CURVE_BANDS] = { 0, 10, 33, 58, 60, 16, 200 };

/*
 * All currently known UM-series boards expose the same EC register layout.
 * A board whose firmware lays these out differently must not reuse this
 * macro; give it explicit values so it cannot reach the wrong addresses.
 */
#define MF_UM_LAYOUT							\
	.chip_id		= MF_CHIP_ID_IT5571,			\
	.sio_port		= 0x2E,					\
	.ec_temp_cpu		= 0x09,					\
	.ec_temp_board		= 0x05,					\
	.ec_temp_vrm		= 0x04,					\
	.ec_profile		= 0x2F,					\
	.profile_quiet		= 0x00,					\
	.profile_balanced	= 0xB1,					\
	.profile_performance	= 0xB2,					\
	.xram_curve_base	= 0x0310,				\
	.xram_curve_slope	= 0x08B0,				\
	.xram_tach_cpu		= 0x181E,				\
	.xram_tach_sys		= 0x1820,				\
	.max_fan_code		= 51,					\
	.min_fan_code		= 5,					\
	.fallback_bases		= mf_um_fallback_bases,			\
	.fallback_slopes	= mf_um_fallback_slopes

static const struct minisforum_board_config board_um790_pro = {
	.board_name = "F7BSC",
	.description = "Minisforum UM790 Pro (Venus series)",
	.num_fans = 2,
	MF_UM_LAYOUT,
};

static const struct minisforum_board_config board_um780_xtx = {
	.board_name = "F7BSD",
	.description = "Minisforum UM780 XTX",
	.num_fans = 2,
	MF_UM_LAYOUT,
};

static const struct minisforum_board_config board_um880_pro = {
	.board_name = "F7BSI",
	.description = "Minisforum EliteMini UM880 Pro",
	.num_fans = 2,
	MF_UM_LAYOUT,
};

static const struct minisforum_board_config board_um890_pro = {
	.board_name = "F7BSW",
	.description = "Minisforum UM890 Pro",
	.num_fans = 2,
	MF_UM_LAYOUT,
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

	dev_info(dev, "%s\n", data->board->description);
	return 0;
}

static struct platform_driver minisforum_driver = {
	.probe = minisforum_probe,
	.driver = {
		.name = DRV_NAME,
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
