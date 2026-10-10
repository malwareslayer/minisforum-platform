/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minisforum Platform Profile Driver (TDP / Thermal Profiles & CPU Coordination)
 *
 * Exposes /sys/firmware/acpi/platform_profile for GNOME, KDE, and power-profiles-daemon,
 * and dynamically synchronizes cpufreq governors and frequency scaling limits across
 * amd-pstate=guided and generic cpufreq governors.
 */

#include <linux/umh.h>
#include "minisforum.h"

/* Configurable module parameters */
static bool manage_cpufreq = true;
module_param(manage_cpufreq, bool, 0644);
MODULE_PARM_DESC(manage_cpufreq, "Synchronize cpufreq governor and frequency limits with platform profiles (default: true)");

static char gov_low_power[16] = "powersave";
module_param_string(gov_low_power, gov_low_power, sizeof(gov_low_power), 0644);
MODULE_PARM_DESC(gov_low_power, "CPU governor for low-power profile (default: 'powersave')");

static char gov_balanced[16] = "schedutil";
module_param_string(gov_balanced, gov_balanced, sizeof(gov_balanced), 0644);
MODULE_PARM_DESC(gov_balanced, "CPU governor for balanced profile (default: 'schedutil')");

static char gov_performance[16] = "performance";
module_param_string(gov_performance, gov_performance, sizeof(gov_performance), 0644);
MODULE_PARM_DESC(gov_performance, "CPU governor for performance profile (default: 'performance')");

static unsigned int freq_low_power_max;
module_param(freq_low_power_max, uint, 0644);
MODULE_PARM_DESC(freq_low_power_max, "Max CPU frequency in kHz for low-power profile (0: unconstrained)");

static unsigned int freq_balanced_max;
module_param(freq_balanced_max, uint, 0644);
MODULE_PARM_DESC(freq_balanced_max, "Max CPU frequency in kHz for balanced profile (0: unconstrained)");

static unsigned int freq_performance_max;
module_param(freq_performance_max, uint, 0644);
MODULE_PARM_DESC(freq_performance_max, "Max CPU frequency in kHz for performance profile (0: unconstrained)");

/* Worker function to synchronize cpufreq governors and limits */
static void mf_cpufreq_worker(struct work_struct *work)
{
	struct minisforum_data *data = container_of(work, struct minisforum_data, cpufreq_work);
	const char *target_gov = gov_balanced;
	unsigned int target_max_freq = freq_balanced_max;
	char max_str[16];
	char *argv[7];
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		NULL
	};

	if (!manage_cpufreq)
		return;

	switch (data->pending_profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		target_gov = gov_low_power;
		target_max_freq = freq_low_power_max;
		break;
	case PLATFORM_PROFILE_BALANCED:
		target_gov = gov_balanced;
		target_max_freq = freq_balanced_max;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		target_gov = gov_performance;
		target_max_freq = freq_performance_max;
		break;
	default:
		return;
	}

	snprintf(max_str, sizeof(max_str), "%u", target_max_freq);

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = "gov=\"$1\"; max=\"$2\"; "
		  "for cpu in /sys/devices/system/cpu/cpu[0-9]*; do "
		  "  [ -n \"$gov\" ] && [ -f \"$cpu/cpufreq/scaling_governor\" ] && echo \"$gov\" > \"$cpu/cpufreq/scaling_governor\" 2>/dev/null; "
		  "  if [ \"$max\" != \"0\" ] && [ -n \"$max\" ] && [ -f \"$cpu/cpufreq/scaling_max_freq\" ]; then "
		  "    echo \"$max\" > \"$cpu/cpufreq/scaling_max_freq\" 2>/dev/null; "
		  "  elif [ -f \"$cpu/cpufreq/cpuinfo_max_freq\" ] && [ -f \"$cpu/cpufreq/scaling_max_freq\" ]; then "
		  "    cat \"$cpu/cpufreq/cpuinfo_max_freq\" > \"$cpu/cpufreq/scaling_max_freq\" 2>/dev/null; "
		  "  fi; "
		  "done";
	argv[3] = "_";
	argv[4] = (char *)target_gov;
	argv[5] = max_str;
	argv[6] = NULL;

	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	dev_info(&data->pdev->dev, "Synchronized cpufreq: profile=%d governor=%s max_freq=%u kHz\n",
		 data->pending_profile, target_gov, target_max_freq);
}

void minisforum_profile_sync_cpufreq(struct minisforum_data *data, enum platform_profile_option profile)
{
	if (!manage_cpufreq)
		return;

	data->pending_profile = profile;
	queue_work(system_dfl_wq, &data->cpufreq_work);
}

/* Sysfs Device Attributes */
static ssize_t manage_cpufreq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", manage_cpufreq ? 1 : 0);
}

static ssize_t manage_cpufreq_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	manage_cpufreq = val;
	if (manage_cpufreq && data)
		minisforum_profile_sync_cpufreq(data, data->current_profile);

	return count;
}
static DEVICE_ATTR_RW(manage_cpufreq);

static ssize_t gov_low_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", gov_low_power);
}

static ssize_t gov_low_power_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%15s", gov_low_power) != 1)
		return -EINVAL;

	if (data && data->current_profile == PLATFORM_PROFILE_LOW_POWER)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_LOW_POWER);

	return count;
}
static DEVICE_ATTR_RW(gov_low_power);

static ssize_t gov_balanced_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", gov_balanced);
}

static ssize_t gov_balanced_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%15s", gov_balanced) != 1)
		return -EINVAL;

	if (data && data->current_profile == PLATFORM_PROFILE_BALANCED)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_BALANCED);

	return count;
}
static DEVICE_ATTR_RW(gov_balanced);

static ssize_t gov_performance_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", gov_performance);
}

static ssize_t gov_performance_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%15s", gov_performance) != 1)
		return -EINVAL;

	if (data && data->current_profile == PLATFORM_PROFILE_PERFORMANCE)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_PERFORMANCE);

	return count;
}
static DEVICE_ATTR_RW(gov_performance);

static ssize_t freq_low_power_max_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_low_power_max);
}

static ssize_t freq_low_power_max_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_low_power_max = val;
	if (data && data->current_profile == PLATFORM_PROFILE_LOW_POWER)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_LOW_POWER);

	return count;
}
static DEVICE_ATTR_RW(freq_low_power_max);

static ssize_t freq_balanced_max_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_balanced_max);
}

static ssize_t freq_balanced_max_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_balanced_max = val;
	if (data && data->current_profile == PLATFORM_PROFILE_BALANCED)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_BALANCED);

	return count;
}
static DEVICE_ATTR_RW(freq_balanced_max);

static ssize_t freq_performance_max_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_performance_max);
}

static ssize_t freq_performance_max_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_performance_max = val;
	if (data && data->current_profile == PLATFORM_PROFILE_PERFORMANCE)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_PERFORMANCE);

	return count;
}
static DEVICE_ATTR_RW(freq_performance_max);

static struct attribute *mf_platform_attrs[] = {
	&dev_attr_manage_cpufreq.attr,
	&dev_attr_gov_low_power.attr,
	&dev_attr_gov_balanced.attr,
	&dev_attr_gov_performance.attr,
	&dev_attr_freq_low_power_max.attr,
	&dev_attr_freq_balanced_max.attr,
	&dev_attr_freq_performance_max.attr,
	NULL,
};

static const struct attribute_group mf_platform_attr_group = {
	.attrs = mf_platform_attrs,
};

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

	/* Synchronize CPU governor and frequency limits */
	minisforum_profile_sync_cpufreq(data, profile);

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

	INIT_WORK(&data->cpufreq_work, mf_cpufreq_worker);

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

	ret = sysfs_create_group(&data->pdev->dev.kobj, &mf_platform_attr_group);
	if (ret)
		dev_warn(&data->pdev->dev, "Failed to create sysfs platform attribute group (%d)\n", ret);

	/* Synchronize CPU governor for initial detected profile */
	minisforum_profile_sync_cpufreq(data, data->current_profile);

	dev_info(&data->pdev->dev, "Platform profile registered successfully\n");
	return 0;
}

void minisforum_profile_exit(struct minisforum_data *data)
{
	sysfs_remove_group(&data->pdev->dev.kobj, &mf_platform_attr_group);
	cancel_work_sync(&data->cpufreq_work);

	if (data->pp_dev) {
		platform_profile_remove(data->pp_dev);
		data->pp_dev = NULL;
	}
}
