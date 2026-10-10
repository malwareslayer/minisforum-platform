/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minisforum Platform Profile Driver (TDP / Thermal Profiles)
 *
 * Exposes /sys/firmware/acpi/platform_profile for GNOME, KDE,
 * and power-profiles-daemon.
 */

#include "minisforum.h"

static int mf_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int mf_profile_get(struct device *dev, enum platform_profile_option *profile)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (!data)
		return -ENODEV;

	mutex_lock(&data->lock);
	*profile = data->current_profile;
	mutex_unlock(&data->lock);

	return 0;
}

static int mf_profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	u8 ec_code;
	int ret = 0;

	if (!data)
		return -ENODEV;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		ec_code = MF_EC_PROFILE_QUIET;
		dev_info(dev, "Switching to Low Power (Quiet / Eco 35W)\n");
		break;
	case PLATFORM_PROFILE_BALANCED:
		ec_code = MF_EC_PROFILE_BALANCED;
		dev_info(dev, "Switching to Balanced (Standard 54W)\n");
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		ec_code = MF_EC_PROFILE_PERFORMANCE;
		dev_info(dev, "Switching to Performance (Turbo 65W)\n");
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->lock);

	/* Write the corresponding profile selector to the EC */
	ret = minisforum_ec_write(MF_EC_REG_CPU_PROFILE, ec_code);
	if (ret) {
		dev_warn(dev, "Failed to update EC profile register (err %d)\n", ret);
		/* Still update internal state so userspace governor continues */
	}

	data->current_profile = profile;
	mutex_unlock(&data->lock);

	/* Synchronize hardware EC fan curve to match active profile */
	minisforum_hwmon_apply_profile_curve(data, profile);

	return 0;
}

static const struct platform_profile_ops mf_profile_ops = {
	.probe = mf_profile_probe,
	.profile_get = mf_profile_get,
	.profile_set = mf_profile_set,
};

int minisforum_profile_init(struct minisforum_data *data)
{
	u8 initial_ec_profile = 0;
	int ret;

	/* Read the initial profile from the EC */
	ret = minisforum_ec_read(MF_EC_REG_CPU_PROFILE, &initial_ec_profile);
	if (ret == 0) {
		switch (initial_ec_profile) {
		case MF_EC_PROFILE_QUIET:
			data->current_profile = PLATFORM_PROFILE_LOW_POWER;
			break;
		case MF_EC_PROFILE_PERFORMANCE:
			data->current_profile = PLATFORM_PROFILE_PERFORMANCE;
			break;
		case MF_EC_PROFILE_BALANCED:
		default:
			data->current_profile = PLATFORM_PROFILE_BALANCED;
			break;
		}
	} else {
		data->current_profile = PLATFORM_PROFILE_BALANCED;
	}

	data->pp_dev = platform_profile_register(&data->pdev->dev, "minisforum",
						 data, &mf_profile_ops);
	if (IS_ERR(data->pp_dev)) {
		dev_err(&data->pdev->dev, "Failed to register platform_profile: %ld\n",
			PTR_ERR(data->pp_dev));
		return PTR_ERR(data->pp_dev);
	}

	dev_info(&data->pdev->dev, "Platform profile registered successfully\n");
	return 0;
}

void minisforum_profile_exit(struct minisforum_data *data)
{
	if (data->pp_dev) {
		platform_profile_remove(data->pp_dev);
		data->pp_dev = NULL;
	}
}
