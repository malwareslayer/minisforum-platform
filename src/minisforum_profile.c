// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minisforum Platform Profile Driver (TDP / Thermal Profiles & CPU Coordination)
 *
 * Exposes /sys/firmware/acpi/platform_profile for GNOME, KDE, and power-profiles-daemon,
 * and dynamically synchronizes in-kernel cpufreq QoS limits and hardware CPPC EPP hints.
 */

#include <linux/cpufreq.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "minisforum.h"

/* Configurable module parameters */
static bool manage_cpufreq = true;
module_param(manage_cpufreq, bool, 0644);
MODULE_PARM_DESC(manage_cpufreq, "Synchronize in-kernel frequency QoS and CPPC EPP with platform profiles (default: true)");

static unsigned int freq_low_power_min = 1754353;
module_param(freq_low_power_min, uint, 0644);
MODULE_PARM_DESC(freq_low_power_min, "Min CPU frequency in kHz for low-power profile");

static unsigned int freq_low_power_max = 2800000;
module_param(freq_low_power_max, uint, 0644);
MODULE_PARM_DESC(freq_low_power_max, "Max CPU frequency in kHz for low-power profile");

static unsigned int freq_balanced_min = 1754353;
module_param(freq_balanced_min, uint, 0644);
MODULE_PARM_DESC(freq_balanced_min, "Min CPU frequency in kHz for balanced profile");

static unsigned int freq_balanced_max;
module_param(freq_balanced_max, uint, 0644);
MODULE_PARM_DESC(freq_balanced_max, "Max CPU frequency in kHz for balanced profile (0: unconstrained)");

static unsigned int freq_performance_min = 2631530;
module_param(freq_performance_min, uint, 0644);
MODULE_PARM_DESC(freq_performance_min, "Min CPU frequency in kHz for performance profile");

static unsigned int freq_performance_max;
module_param(freq_performance_max, uint, 0644);
MODULE_PARM_DESC(freq_performance_max, "Max CPU frequency in kHz for performance profile (0: unconstrained)");

static bool mf_detect_amd_pstate(void)
{
	const char *driver = cpufreq_get_current_driver();

	if (!driver)
		return false;

	return (strncmp(driver, "amd-pstate", 10) == 0 ||
		strncmp(driver, "amd_pstate", 10) == 0);
}

/* Apply frequency constraints and energy performance preference */
static void mf_apply_profile_qos(struct minisforum_data *data,
				 enum platform_profile_option profile)
{
	unsigned int target_min, target_max;
	const char *target_epp_str;
	u64 target_epp;
	int cpu;

	if (!data->qos_initialized || !manage_cpufreq || !data->has_amd_pstate)
		return;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		target_min = freq_low_power_min;
		target_max = freq_low_power_max;
		target_epp = 0xFF; /* CPPC Power */
		target_epp_str = "power";
		break;
	case PLATFORM_PROFILE_BALANCED:
		target_min = freq_balanced_min;
		target_max = freq_balanced_max ?
			freq_balanced_max : FREQ_QOS_MAX_DEFAULT_VALUE;
		target_epp = 0x80; /* CPPC Balance Performance */
		target_epp_str = "balance_performance";
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		target_min = freq_performance_min;
		target_max = freq_performance_max ?
			freq_performance_max : FREQ_QOS_MAX_DEFAULT_VALUE;
		target_epp = 0x00; /* CPPC Peak Performance */
		target_epp_str = "performance";
		break;
	default:
		return;
	}

	data->sync_in_progress = true;

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;

		if (freq_qos_request_active(&data->qos_min_req[cpu]))
			freq_qos_update_request(&data->qos_min_req[cpu], target_min);
		if (freq_qos_request_active(&data->qos_max_req[cpu]))
			freq_qos_update_request(&data->qos_max_req[cpu], target_max);
		cppc_set_epp(cpu, target_epp);

		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			store_energy_performance_preference(policy,
							    target_epp_str,
							    strlen(target_epp_str));
			cpufreq_cpu_put(policy);
		}
	}

	dev_info(&data->pdev->dev,
		 "Applied in-kernel QoS: profile=%d [%u - %u kHz] EPP=%s (0x%llx)\n",
		 profile, target_min, target_max, target_epp_str, target_epp);

	data->sync_in_progress = false;
}

void minisforum_profile_sync_cpufreq(struct minisforum_data *data,
				     enum platform_profile_option profile)
{
	mf_apply_profile_qos(data, profile);
}

/*
 * Bidirectional check: Detects if frequency constraints or governors were changed
 * externally and aligns the ACPI platform profile accordingly.
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

	/* Determine target profile from active governor and constraints */
	if (strcmp(current_gov, "performance") == 0) {
		target_profile = PLATFORM_PROFILE_PERFORMANCE;
		ec_code = MF_EC_PROFILE_PERFORMANCE;
	} else if (strcmp(current_gov, "powersave") == 0 &&
		   freq_low_power_max > 0 && max_freq <= freq_low_power_max + 100000) {
		target_profile = PLATFORM_PROFILE_LOW_POWER;
		ec_code = MF_EC_PROFILE_QUIET;
	} else {
		target_profile = PLATFORM_PROFILE_BALANCED;
		ec_code = MF_EC_PROFILE_BALANCED;
	}

	if (target_profile != data->current_profile) {
		dev_info(&data->pdev->dev,
			 "External policy change detected ('%s', max %u kHz) -> aligning platform profile to %d\n",
			 current_gov, max_freq, target_profile);

		mutex_lock(&data->lock);
		data->current_profile = target_profile;
		minisforum_ec_write(MF_EC_REG_CPU_PROFILE, ec_code);
		mutex_unlock(&data->lock);

		/* Update fan curve */
		minisforum_hwmon_apply_profile_curve(data, target_profile);

		/* Notify userspace / desktop environment */
		if (data->pp_dev)
			platform_profile_notify(data->pp_dev);
	}
}

/* Sysfs Device Attributes */
static ssize_t manage_cpufreq_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", manage_cpufreq ? 1 : 0);
}

static ssize_t manage_cpufreq_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
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

static ssize_t freq_low_power_min_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_low_power_min);
}

static ssize_t freq_low_power_min_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
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

static ssize_t freq_low_power_max_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_low_power_max);
}

static ssize_t freq_low_power_max_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
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

static ssize_t freq_balanced_min_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_balanced_min);
}

static ssize_t freq_balanced_min_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
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

static ssize_t freq_balanced_max_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_balanced_max);
}

static ssize_t freq_balanced_max_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
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

static ssize_t freq_performance_min_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_performance_min);
}

static ssize_t freq_performance_min_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
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

static ssize_t freq_performance_max_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", freq_performance_max);
}

static ssize_t freq_performance_max_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
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
	&dev_attr_freq_low_power_min.attr,
	&dev_attr_freq_low_power_max.attr,
	&dev_attr_freq_balanced_min.attr,
	&dev_attr_freq_balanced_max.attr,
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
	if (ret)
		dev_warn(dev, "Failed to update EC profile register (err %d)\n", ret);

	data->current_profile = profile;
	mutex_unlock(&data->lock);

	/* Synchronize hardware EC fan curve to match active profile */
	minisforum_hwmon_apply_profile_curve(data, profile);

	/* Synchronize in-kernel frequency QoS and hardware CPPC EPP */
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
	int ret, cpu;

	/* Detect presence of AMD P-State architecture */
	data->has_amd_pstate = mf_detect_amd_pstate();
	data->sync_in_progress = false;

	if (!data->has_amd_pstate) {
		manage_cpufreq = false;
		dev_info(&data->pdev->dev,
			 "AMD P-State not detected. In-kernel cpufreq QoS disabled.\n");
	} else {
		dev_info(&data->pdev->dev,
			 "AMD P-State detected. Native in-kernel freq_qos and CPPC EPP active.\n");
	}

	/* Initialize in-kernel Frequency QoS requests across all online CPUs */
	data->qos_min_req = kcalloc(nr_cpu_ids, sizeof(*data->qos_min_req), GFP_KERNEL);
	data->qos_max_req = kcalloc(nr_cpu_ids, sizeof(*data->qos_max_req), GFP_KERNEL);
	if (!data->qos_min_req || !data->qos_max_req) {
		kfree(data->qos_min_req);
		kfree(data->qos_max_req);
		data->qos_min_req = NULL;
		data->qos_max_req = NULL;
		data->qos_initialized = false;
	} else {
		for_each_online_cpu(cpu) {
			struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

			if (policy) {
				freq_qos_add_request(&policy->constraints,
						     &data->qos_min_req[cpu],
						     FREQ_QOS_MIN,
						     FREQ_QOS_MIN_DEFAULT_VALUE);
				freq_qos_add_request(&policy->constraints,
						     &data->qos_max_req[cpu],
						     FREQ_QOS_MAX,
						     FREQ_QOS_MAX_DEFAULT_VALUE);
				cpufreq_cpu_put(policy);
			}
		}
		data->qos_initialized = true;
	}

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
		dev_warn(&data->pdev->dev,
			 "Failed to create sysfs platform attribute group (%d)\n", ret);

	/* Synchronize CPU governor and QoS for initial detected profile */
	if (data->has_amd_pstate)
		minisforum_profile_sync_cpufreq(data, data->current_profile);

	dev_info(&data->pdev->dev, "Platform profile registered successfully\n");
	return 0;
}

void minisforum_profile_exit(struct minisforum_data *data)
{
	int cpu;

	sysfs_remove_group(&data->pdev->dev.kobj, &mf_platform_attr_group);

	/* Safely release all in-kernel QoS requests */
	if (data->qos_initialized) {
		for_each_possible_cpu(cpu) {
			if (freq_qos_request_active(&data->qos_min_req[cpu]))
				freq_qos_remove_request(&data->qos_min_req[cpu]);
			if (freq_qos_request_active(&data->qos_max_req[cpu]))
				freq_qos_remove_request(&data->qos_max_req[cpu]);
		}
		kfree(data->qos_min_req);
		kfree(data->qos_max_req);
		data->qos_min_req = NULL;
		data->qos_max_req = NULL;
		data->qos_initialized = false;
	}

	if (data->pp_dev) {
		platform_profile_remove(data->pp_dev);
		data->pp_dev = NULL;
	}
}
