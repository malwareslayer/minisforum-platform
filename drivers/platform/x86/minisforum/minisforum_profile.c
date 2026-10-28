// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Platform Profile Driver (TDP & Thermal Profiles)
 *
 * Exposes /sys/firmware/acpi/platform_profile for GNOME, KDE and
 * power-profiles-daemon, translating a profile selection into the CPU power
 * profile selector of the board's Embedded Controller.
 */

#include "minisforum.h"

static int mf_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int mf_profile_get(struct device *dev,
			  enum platform_profile_option *profile)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (!data)
		return -ENODEV;

	mutex_lock(&data->lock);
	*profile = data->current_profile;
	mutex_unlock(&data->lock);

	return 0;
}

int minisforum_profile_set(struct minisforum_data *data,
			   enum platform_profile_option profile)
{
	u8 ec_code;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		ec_code = MF_EC_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		ec_code = MF_EC_PROFILE_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		ec_code = MF_EC_PROFILE_PERFORMANCE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->lock);
	ret = ec_write(MF_EC_REG_CPU_PROFILE, ec_code);
	if (!ret)
		data->current_profile = profile;
	mutex_unlock(&data->lock);

	if (ret)
		return ret;

	dev_dbg(data->dev, "platform profile set to %d\n", profile);
	return 0;
}

static int mf_profile_set(struct device *dev,
			  enum platform_profile_option profile)
{
	return minisforum_profile_set(dev_get_drvdata(dev), profile);
}

static const struct platform_profile_ops mf_profile_ops = {
	.probe = mf_profile_probe,
	.profile_get = mf_profile_get,
	.profile_set = mf_profile_set,
};

int minisforum_profile_init(struct device *dev, struct minisforum_data *data)
{
	u8 initial_ec_profile = 0;
	struct device *pp_dev;

	if (!ec_read(MF_EC_REG_CPU_PROFILE, &initial_ec_profile)) {
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

	pp_dev = devm_platform_profile_register(dev, "minisforum", data,
						&mf_profile_ops);
	if (IS_ERR(pp_dev))
		return dev_err_probe(dev, PTR_ERR(pp_dev),
				     "failed to register platform profile\n");

	return 0;
}
