/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minisforum Platform Profile Driver (TDP / Thermal Profiles & CPU Coordination)
 *
 * Exposes /sys/firmware/acpi/platform_profile for GNOME, KDE, and power-profiles-daemon,
 * and dynamically synchronizes cpufreq governors and frequency scaling limits across
 * amd-pstate=guided, amd-pstate=active (EPP), and generic governors.
 */

#include <linux/umh.h>
#include <linux/cpufreq.h>
#include <linux/string.h>
#include "minisforum.h"

/* Configurable module parameters */
static bool manage_cpufreq = true;
module_param(manage_cpufreq, bool, 0644);
MODULE_PARM_DESC(manage_cpufreq, "Synchronize cpufreq governor and frequency limits with platform profiles (default: true)");

/* Low-Power Tuning (ThinkPad-style: dynamic schedutil bounded between 1.9 GHz and 2.9 GHz) */
static char gov_low_power[16] = "schedutil";
module_param_string(gov_low_power, gov_low_power, sizeof(gov_low_power), 0644);
MODULE_PARM_DESC(gov_low_power, "CPU governor for low-power profile (default: 'schedutil')");

static unsigned int freq_low_power_min = 1900000;
module_param(freq_low_power_min, uint, 0644);
MODULE_PARM_DESC(freq_low_power_min, "Min CPU frequency in kHz for low-power profile (default: 1900000 / 1.9 GHz)");

static unsigned int freq_low_power_max = 2900000;
module_param(freq_low_power_max, uint, 0644);
MODULE_PARM_DESC(freq_low_power_max, "Max CPU frequency in kHz for low-power profile (default: 2900000 / 2.9 GHz)");

/* Balanced Tuning (Dynamic schedutil from hardware base floor to maximum boost) */
static char gov_balanced[16] = "schedutil";
module_param_string(gov_balanced, gov_balanced, sizeof(gov_balanced), 0644);
MODULE_PARM_DESC(gov_balanced, "CPU governor for balanced profile (default: 'schedutil')");

static unsigned int freq_balanced_min;
module_param(freq_balanced_min, uint, 0644);
MODULE_PARM_DESC(freq_balanced_min, "Min CPU frequency in kHz for balanced profile (0: unconstrained floor)");

static unsigned int freq_balanced_max;
module_param(freq_balanced_max, uint, 0644);
MODULE_PARM_DESC(freq_balanced_max, "Max CPU frequency in kHz for balanced profile (0: unconstrained boost)");

/* Performance Tuning (Performance governor with high 2.8 GHz baseline floor) */
static char gov_performance[16] = "performance";
module_param_string(gov_performance, gov_performance, sizeof(gov_performance), 0644);
MODULE_PARM_DESC(gov_performance, "CPU governor for performance profile (default: 'performance')");

static unsigned int freq_performance_min = 2800000;
module_param(freq_performance_min, uint, 0644);
MODULE_PARM_DESC(freq_performance_min, "Min CPU frequency in kHz for performance profile (default: 2800000 / 2.8 GHz)");

static unsigned int freq_performance_max;
module_param(freq_performance_max, uint, 0644);
MODULE_PARM_DESC(freq_performance_max, "Max CPU frequency in kHz for performance profile (0: unconstrained boost)");

static bool mf_detect_amd_pstate(void)
{
	const char *driver = cpufreq_get_current_driver();

	if (!driver)
		return false;

	return (strcmp(driver, "amd-pstate") == 0 ||
		strcmp(driver, "amd_pstate_epp") == 0);
}

/* Worker function to synchronize cpufreq governors, limits, and EPP hints */
static void mf_cpufreq_worker(struct work_struct *work)
{
	struct minisforum_data *data = container_of(work, struct minisforum_data, cpufreq_work);
	const char *target_gov = gov_balanced;
	const char *target_epp = "balance_performance";
	unsigned int target_min_freq = freq_balanced_min;
	unsigned int target_max_freq = freq_balanced_max;
	char min_str[16];
	char max_str[16];
	char *argv[9];
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		NULL
	};

	if (!manage_cpufreq || !data->has_amd_pstate) {
		data->sync_in_progress = false;
		return;
	}

	switch (data->pending_profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		target_gov = gov_low_power;
		target_min_freq = freq_low_power_min;
		target_max_freq = freq_low_power_max;
		target_epp = "power";
		break;
	case PLATFORM_PROFILE_BALANCED:
		target_gov = gov_balanced;
		target_min_freq = freq_balanced_min;
		target_max_freq = freq_balanced_max;
		target_epp = "balance_performance";
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		target_gov = gov_performance;
		target_min_freq = freq_performance_min;
		target_max_freq = freq_performance_max;
		target_epp = "performance";
		break;
	default:
		data->sync_in_progress = false;
		return;
	}

	snprintf(min_str, sizeof(min_str), "%u", target_min_freq);
	snprintf(max_str, sizeof(max_str), "%u", target_max_freq);

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = "gov=\"$1\"; fmin=\"$2\"; fmax=\"$3\"; epp=\"$4\"; "
		  "for cpu in /sys/devices/system/cpu/cpu[0-9]*; do "
		  "  [ -n \"$gov\" ] && [ -f \"$cpu/cpufreq/scaling_governor\" ] && echo \"$gov\" > \"$cpu/cpufreq/scaling_governor\" 2>/dev/null; "
		  "  if [ \"$fmin\" != \"0\" ] && [ -n \"$fmin\" ] && [ -f \"$cpu/cpufreq/scaling_min_freq\" ]; then "
		  "    echo \"$fmin\" > \"$cpu/cpufreq/scaling_min_freq\" 2>/dev/null; "
		  "  elif [ -f \"$cpu/cpufreq/cpuinfo_min_freq\" ] && [ -f \"$cpu/cpufreq/scaling_min_freq\" ]; then "
		  "    cat \"$cpu/cpufreq/cpuinfo_min_freq\" > \"$cpu/cpufreq/scaling_min_freq\" 2>/dev/null; "
		  "  fi; "
		  "  if [ \"$fmax\" != \"0\" ] && [ -n \"$fmax\" ] && [ -f \"$cpu/cpufreq/scaling_max_freq\" ]; then "
		  "    echo \"$fmax\" > \"$cpu/cpufreq/scaling_max_freq\" 2>/dev/null; "
		  "  elif [ -f \"$cpu/cpufreq/cpuinfo_max_freq\" ] && [ -f \"$cpu/cpufreq/scaling_max_freq\" ]; then "
		  "    cat \"$cpu/cpufreq/cpuinfo_max_freq\" > \"$cpu/cpufreq/scaling_max_freq\" 2>/dev/null; "
		  "  fi; "
		  "  [ -n \"$epp\" ] && [ -f \"$cpu/cpufreq/energy_performance_preference\" ] && echo \"$epp\" > \"$cpu/cpufreq/energy_performance_preference\" 2>/dev/null; "
		  "done; "
		  "exit 0";
	argv[3] = "_";
	argv[4] = (char *)target_gov;
	argv[5] = min_str;
	argv[6] = max_str;
	argv[7] = (char *)target_epp;
	argv[8] = NULL;

	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);

	dev_info(&data->pdev->dev,
		 "Synchronized cpufreq: profile=%d gov=%s [%u - %u kHz] EPP=%s\n",
		 data->pending_profile, target_gov, target_min_freq, target_max_freq, target_epp);

	/* Release sync lock after execution */
	data->sync_in_progress = false;
}

void minisforum_profile_sync_cpufreq(struct minisforum_data *data, enum platform_profile_option profile)
{
	if (!manage_cpufreq || !data->has_amd_pstate)
		return;

	data->sync_in_progress = true;
	data->pending_profile = profile;
	queue_work(system_dfl_wq, &data->cpufreq_work);
}

/*
 * Bidirectional check: Detects if the CPU governor was changed externally
 * and aligns the ACPI platform profile and EC TDP accordingly.
 */
void minisforum_profile_check_governor(struct minisforum_data *data)
{
	struct cpufreq_policy *policy;
	char current_gov[CPUFREQ_NAME_LEN] = {0};
	unsigned int max_freq = 0;
	enum platform_profile_option target_profile;
	u8 ec_code;

	if (!manage_cpufreq || !data->has_amd_pstate || data->sync_in_progress)
		return;

	policy = cpufreq_cpu_get(0);
	if (!policy)
		return;

	if (policy->governor)
		strscpy(current_gov, policy->governor->name, sizeof(current_gov));
	max_freq = policy->max;
	cpufreq_cpu_put(policy);

	if (current_gov[0] == '\0')
		return;

	/* Determine target profile from active governor and frequency constraints */
	if (strcmp(current_gov, "powersave") == 0) {
		target_profile = PLATFORM_PROFILE_LOW_POWER;
		ec_code = MF_EC_PROFILE_QUIET;
	} else if (strcmp(current_gov, "performance") == 0) {
		target_profile = PLATFORM_PROFILE_PERFORMANCE;
		ec_code = MF_EC_PROFILE_PERFORMANCE;
	} else if (strcmp(current_gov, "schedutil") == 0 ||
		   strcmp(current_gov, "ondemand") == 0 ||
		   strcmp(current_gov, "conservative") == 0) {
		/*
		 * If CPU max frequency is capped to low-power threshold (e.g. <= 3.0 GHz),
		 * classify as low-power. Otherwise, classify as balanced.
		 */
		if (freq_low_power_max > 0 && max_freq <= freq_low_power_max + 100000) {
			target_profile = PLATFORM_PROFILE_LOW_POWER;
			ec_code = MF_EC_PROFILE_QUIET;
		} else {
			target_profile = PLATFORM_PROFILE_BALANCED;
			ec_code = MF_EC_PROFILE_BALANCED;
		}
	} else {
		return;
	}

	if (target_profile != data->current_profile) {
		dev_info(&data->pdev->dev,
			 "External governor '%s' detected -> aligning platform profile to %d\n",
			 current_gov, target_profile);

		mutex_lock(&data->lock);
		data->current_profile = target_profile;
		minisforum_ec_write(MF_EC_REG_CPU_PROFILE, ec_code);
		mutex_unlock(&data->lock);

		/* Update fan hysteresis curve */
		minisforum_hwmon_apply_profile_curve(data, target_profile);

		/* Notify userspace / desktop environment of profile shift */
		if (data->pp_dev)
			platform_profile_notify(data->pp_dev);
	}
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
	if (manage_cpufreq && data && data->has_amd_pstate)
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

static ssize_t freq_low_power_min_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_low_power_min);
}

static ssize_t freq_low_power_min_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_low_power_min = val;
	if (data && data->current_profile == PLATFORM_PROFILE_LOW_POWER)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_LOW_POWER);

	return count;
}
static DEVICE_ATTR_RW(freq_low_power_min);

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

static ssize_t freq_balanced_min_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_balanced_min);
}

static ssize_t freq_balanced_min_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_balanced_min = val;
	if (data && data->current_profile == PLATFORM_PROFILE_BALANCED)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_BALANCED);

	return count;
}
static DEVICE_ATTR_RW(freq_balanced_min);

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

static ssize_t freq_performance_min_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_performance_min);
}

static ssize_t freq_performance_min_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct minisforum_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return ret;

	freq_performance_min = val;
	if (data && data->current_profile == PLATFORM_PROFILE_PERFORMANCE)
		minisforum_profile_sync_cpufreq(data, PLATFORM_PROFILE_PERFORMANCE);

	return count;
}
static DEVICE_ATTR_RW(freq_performance_min);

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
	&dev_attr_freq_low_power_min.attr,
	&dev_attr_freq_low_power_max.attr,
	&dev_attr_gov_balanced.attr,
	&dev_attr_freq_balanced_min.attr,
	&dev_attr_freq_balanced_max.attr,
	&dev_attr_gov_performance.attr,
	&dev_attr_freq_performance_min.attr,
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
	}

	data->current_profile = profile;
	mutex_unlock(&data->lock);

	/* Synchronize hardware EC fan curve to match active profile */
	minisforum_hwmon_apply_profile_curve(data, profile);

	/* Synchronize CPU governor, limits, and EPP hints */
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

	/* Detect presence of AMD P-State architecture */
	data->has_amd_pstate = mf_detect_amd_pstate();
	data->sync_in_progress = false;

	if (!data->has_amd_pstate) {
		manage_cpufreq = false;
		dev_info(&data->pdev->dev,
			 "AMD P-State not detected. Autonomous cpufreq management disabled.\n");
	} else {
		dev_info(&data->pdev->dev,
			 "AMD P-State detected. Autonomous cpufreq and dynamic governor active.\n");
	}

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
	if (data->has_amd_pstate)
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
