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

static unsigned int freq_low_power_min;
module_param(freq_low_power_min, uint, 0644);
MODULE_PARM_DESC(freq_low_power_min, "Min CPU frequency in kHz for low-power profile (0: hardware baseline)");

static unsigned int freq_low_power_max = 3200000;
module_param(freq_low_power_max, uint, 0644);
MODULE_PARM_DESC(freq_low_power_max, "Max CPU frequency in kHz for low-power profile");

static unsigned int freq_balanced_min;
module_param(freq_balanced_min, uint, 0644);
MODULE_PARM_DESC(freq_balanced_min, "Min CPU frequency in kHz for balanced profile (0: hardware baseline)");

static unsigned int freq_balanced_max;
module_param(freq_balanced_max, uint, 0644);
MODULE_PARM_DESC(freq_balanced_max, "Max CPU frequency in kHz for balanced profile (0: unconstrained)");

static unsigned int freq_performance_min = 2800000;
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

static u64 mf_get_max_highest_perf(void)
{
	u64 max_perf = 0;
	int cpu;

	for_each_online_cpu(cpu) {
		u64 perf = 0;

		if (cppc_get_highest_perf(cpu, &perf) == 0 && perf > max_perf)
			max_perf = perf;
	}

	return max_perf;
}

/* Apply frequency constraints and energy performance preference */
static void mf_apply_profile_qos(struct minisforum_data *data,
				 enum platform_profile_option profile)
{
	u64 max_perf;
	int cpu;

	if (!data->qos_initialized || !manage_cpufreq || !data->has_amd_pstate)
		return;

	max_perf = mf_get_max_highest_perf();

	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;
		unsigned int target_min, target_max;
		u64 target_epp, cpu_perf = 0;
		bool is_gold = false, is_silver = false;

		if (max_perf > 0 && cppc_get_highest_perf(cpu, &cpu_perf) == 0) {
			if (cpu_perf == max_perf)
				is_gold = true;
			else if (cpu_perf + 16 >= max_perf)
				is_silver = true;
		}

		switch (profile) {
		case PLATFORM_PROFILE_LOW_POWER:
			if (is_gold) {
				target_min = freq_low_power_min ?
					freq_low_power_min : FREQ_QOS_MIN_DEFAULT_VALUE;
				target_max = 3200000;
				target_epp = 0x80; /* balance_performance */
			} else if (is_silver) {
				target_min = freq_low_power_min ?
					freq_low_power_min : FREQ_QOS_MIN_DEFAULT_VALUE;
				target_max = 2600000;
				target_epp = 0xC0; /* balance_power */
			} else {
				target_min = freq_low_power_min ?
					freq_low_power_min : FREQ_QOS_MIN_DEFAULT_VALUE;
				target_max = 2000000;
				target_epp = 0xFF; /* power */
			}
			if (freq_low_power_max)
				target_max = min(target_max, freq_low_power_max);
			break;

		case PLATFORM_PROFILE_BALANCED:
			if (is_gold) {
				target_min = max(freq_balanced_min, 3000000U);
				target_max = freq_balanced_max ?
					freq_balanced_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x00; /* performance: lag-free interactive UI */
			} else if (is_silver) {
				target_min = max(freq_balanced_min, 2600000U);
				target_max = freq_balanced_max ?
					freq_balanced_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x20; /* 32: aggressive priority to 4.0 GHz */
			} else {
				target_min = max(freq_balanced_min, 2400000U);
				target_max = freq_balanced_max ?
					freq_balanced_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x30; /* 48: responsive 28W - 54W sweet spot */
			}
			break;

		case PLATFORM_PROFILE_PERFORMANCE:
			if (is_gold) {
				target_min = max(freq_performance_min, 3400000U);
				target_max = freq_performance_max ?
					freq_performance_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x00; /* performance */
			} else if (is_silver) {
				target_min = max(freq_performance_min, 3000000U);
				target_max = freq_performance_max ?
					freq_performance_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x00; /* performance */
			} else {
				target_min = max(freq_performance_min, 2800000U);
				target_max = freq_performance_max ?
					freq_performance_max : FREQ_QOS_MAX_DEFAULT_VALUE;
				target_epp = 0x00; /* performance */
			}
			break;

		default:
			return;
		}

		if (freq_qos_request_active(&data->qos_min_req[cpu]))
			freq_qos_update_request(&data->qos_min_req[cpu], target_min);
		if (freq_qos_request_active(&data->qos_max_req[cpu]))
			freq_qos_update_request(&data->qos_max_req[cpu], target_max);
		cppc_set_epp(cpu, target_epp);

		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			if (!policy->boost_enabled && policy->boost_supported &&
			    profile != PLATFORM_PROFILE_LOW_POWER)
				cpufreq_boost_set_sw(policy, 1);

			cpufreq_cpu_put(policy);
		}
	}

	/* Synchronize system-wide CPU idle exit latency QoS constraint */
	if (data->latency_qos_initialized) {
		s32 latency_limit;

		switch (profile) {
		case PLATFORM_PROFILE_PERFORMANCE:
			latency_limit = 10; /* Max 10 µs: C1/POLL only */
			break;
		case PLATFORM_PROFILE_BALANCED:
			/* Max 100 µs: allows C1/C2 (18µs), skips C3 (350µs) */
			latency_limit = 100;
			break;
		case PLATFORM_PROFILE_LOW_POWER:
		default:
			/* Allow C3 deep sleep for maximum power savings */
			latency_limit = PM_QOS_DEFAULT_VALUE;
			break;
		}

		cpu_latency_qos_update_request(&data->latency_req, latency_limit);
	}

	dev_info(&data->pdev->dev,
		 "Applied in-kernel QoS: profile=%d (asymmetric 3-tier EPP & latency active)\n",
		 profile);
}

void minisforum_profile_sync_cpufreq(struct minisforum_data *data,
				     enum platform_profile_option profile)
{
	mf_apply_profile_qos(data, profile);
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

int minisforum_profile_set(struct minisforum_data *data,
			   enum platform_profile_option profile)
{
	struct device *dev = &data->pdev->dev;
	u8 ec_code;
	int ret = 0;

	if (!data)
		return -ENODEV;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		ec_code = MF_EC_PROFILE_QUIET;
		dev_info(dev, "Switching to Low Power (Quiet / Eco 15-28W)\n");
		break;
	case PLATFORM_PROFILE_BALANCED:
		ec_code = MF_EC_PROFILE_BALANCED;
		dev_info(dev, "Switching to Balanced (Punchy 28-54W)\n");
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		ec_code = MF_EC_PROFILE_PERFORMANCE;
		dev_info(dev, "Switching to Performance (Unconstrained 54-65W+)\n");
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

static int mf_profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct minisforum_data *data = dev_get_drvdata(dev);

	return minisforum_profile_set(data, profile);
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

	/* Initialize in-kernel CPU idle exit latency PM QoS request */
	cpu_latency_qos_add_request(&data->latency_req, PM_QOS_DEFAULT_VALUE);
	data->latency_qos_initialized = true;

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

	/* Safely release CPU latency QoS request */
	if (data->latency_qos_initialized) {
		cpu_latency_qos_remove_request(&data->latency_req);
		data->latency_qos_initialized = false;
	}

	/* Safely release all in-kernel frequency QoS requests */
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
