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

static enum platform_profile_option
mf_ec_to_profile(const struct minisforum_board_config *cfg, u8 ec_code)
{
	if (ec_code == cfg->profile_quiet)
		return PLATFORM_PROFILE_LOW_POWER;
	if (ec_code == cfg->profile_performance)
		return PLATFORM_PROFILE_PERFORMANCE;

	return PLATFORM_PROFILE_BALANCED;
}

static int mf_profile_to_ec(const struct minisforum_board_config *cfg,
			    enum platform_profile_option profile, u8 *ec_code)
{
	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		*ec_code = cfg->profile_quiet;
		return 0;
	case PLATFORM_PROFILE_BALANCED:
		*ec_code = cfg->profile_balanced;
		return 0;
	case PLATFORM_PROFILE_PERFORMANCE:
		*ec_code = cfg->profile_performance;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int minisforum_profile_set(struct minisforum_data *data,
			   enum platform_profile_option profile)
{
	u8 ec_code, readback;
	int ret;

	ret = mf_profile_to_ec(data->board, profile, &ec_code);
	if (ret)
		return ret;

	mutex_lock(&data->lock);

	ret = ec_write(data->board->ec_profile, ec_code);
	if (ret)
		goto out_unlock;

	data->current_profile = profile;

	/*
	 * A successful ec_write() only means the ACPI transaction completed.
	 * Read the selector back so a firmware that quietly ignores the
	 * request shows up in the log instead of silently doing nothing.
	 */
	if (!ec_read(data->board->ec_profile, &readback) && readback != ec_code)
		dev_warn(data->dev,
			 "EC kept profile 0x%02x, requested 0x%02x\n",
			 readback, ec_code);

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * Re-read the selector from the EC and tell userspace when it no longer
 * matches what the kernel believes. The EC resets this register across
 * suspend and firmware may override it on its own; without the notification
 * tuned/ppd and the desktop power slider keep displaying a stale value.
 */
void minisforum_profile_sync(struct minisforum_data *data)
{
	enum platform_profile_option profile;
	u8 ec_code;

	mutex_lock(&data->lock);

	if (ec_read(data->board->ec_profile, &ec_code))
		goto out_unlock;

	profile = mf_ec_to_profile(data->board, ec_code);
	if (profile == data->current_profile)
		goto out_unlock;

	data->current_profile = profile;
	mutex_unlock(&data->lock);

	dev_info(data->dev, "EC profile is now %d, notifying userspace\n",
		 profile);
	platform_profile_notify(data->pp_dev);
	return;

out_unlock:
	mutex_unlock(&data->lock);
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
	u8 initial_ec_profile;

	/* Report whatever the firmware is actually doing, not an assumption. */
	if (ec_read(data->board->ec_profile, &initial_ec_profile))
		initial_ec_profile = data->board->profile_balanced;

	data->current_profile = mf_ec_to_profile(data->board, initial_ec_profile);

	data->pp_dev = devm_platform_profile_register(dev, "minisforum", data,
						      &mf_profile_ops);
	if (IS_ERR(data->pp_dev))
		return dev_err_probe(dev, PTR_ERR(data->pp_dev),
				     "failed to register platform profile\n");

	return 0;
}
