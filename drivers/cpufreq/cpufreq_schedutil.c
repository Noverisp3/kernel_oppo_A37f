/*
 * drivers/cpufreq/cpufreq_schedutil.c
 *
 * CPUFreq governor that scales frequency based on scheduler utilization.
 *
 * Maps scheduler UTIL_EST demand directly to CPU frequency:
 *   target_freq = max_freq * util / max_util
 *
 * This replaces load-based heuristics with a direct scheduler-driven
 * signal, reducing jank during bursty workloads.
 *
 * Copyright (C) 2026 cinnamon-kernel
 * Based on cpufreq_interactive.c by Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/cpu.h>
#include <linux/mutex.h>

struct sugov_cpu {
	struct timer_list timer;
	struct cpufreq_policy *policy;
	unsigned int target_freq;
	spinlock_t target_freq_lock;
	bool started;
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);

/* Tunables */
struct sugov_tunables {
	unsigned int rate_limit_us;
	unsigned int usage_count;
};

static struct sugov_tunables *common_tunables;

/* Governor lock */
static DEFINE_MUTEX(sugov_lock);

/*
 * Sysfs interface for tunables
 */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", common_tunables->object);		\
}

show_one(rate_limit_us, rate_limit_us);

static ssize_t store_rate_limit_us(struct kobject *kobj,
				   struct attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u", &val) != 1)
		return -EINVAL;

	common_tunables->rate_limit_us = max(val, 1000U);
	return count;
}

define_one_global_rw(rate_limit_us);

static struct attribute *sugov_attrs[] = {
	&rate_limit_us.attr,
	NULL,
};

static struct attribute_group sugov_attr_group = {
	.attrs = sugov_attrs,
	.name = "schedutil",
};

/*
 * Timer callback - evaluates utilization and sets frequency
 */
static void sugov_timer(unsigned long data)
{
	unsigned int cpu = data;
	struct sugov_cpu *sg = &per_cpu(sugov_cpu, cpu);
	struct cpufreq_policy *policy = sg->policy;
	unsigned long util;
	unsigned int target_freq;
	unsigned long flags;

	if (!sg->started)
		return;

	util = sched_cpu_util(cpu);

	if (util == 0) {
		target_freq = policy->min;
	} else {
		target_freq = policy->cpuinfo.max_freq * util / 1024;
	}

	target_freq = clamp(target_freq, policy->min, policy->max);

	spin_lock_irqsave(&sg->target_freq_lock, flags);
	sg->target_freq = target_freq;
	spin_unlock_irqrestore(&sg->target_freq_lock, flags);

	__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_L);

	mod_timer_pinned(&sg->timer,
			 jiffies + usecs_to_jiffies(common_tunables->rate_limit_us));
}

static void sugov_start_timer(int cpu)
{
	struct sugov_cpu *sg = &per_cpu(sugov_cpu, cpu);

	del_timer_sync(&sg->timer);
	sg->timer.expires = jiffies + usecs_to_jiffies(common_tunables->rate_limit_us);
	add_timer_on(&sg->timer, cpu);
}

/*
 * Governor events
 */
static int cpufreq_governor_sugov(struct cpufreq_policy *policy,
				   unsigned int event)
{
	int rc = 0;
	unsigned int j;
	struct sugov_cpu *sg;

	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		if (common_tunables) {
			common_tunables->usage_count++;
			policy->governor_data = common_tunables;
			return 0;
		}

		common_tunables = kzalloc(sizeof(*common_tunables), GFP_KERNEL);
		if (!common_tunables)
			return -ENOMEM;

		common_tunables->rate_limit_us = 20000;
		common_tunables->usage_count = 1;
		policy->governor_data = common_tunables;

		rc = sysfs_create_group(get_governor_parent_kobj(policy),
					&sugov_attr_group);
		if (rc) {
			kfree(common_tunables);
			common_tunables = NULL;
			return rc;
		}
		break;

	case CPUFREQ_GOV_POLICY_EXIT:
		if (--common_tunables->usage_count) {
			policy->governor_data = NULL;
			return 0;
		}

		sysfs_remove_group(get_governor_parent_kobj(policy),
				   &sugov_attr_group);
		kfree(common_tunables);
		common_tunables = NULL;
		policy->governor_data = NULL;
		break;

	case CPUFREQ_GOV_START:
		mutex_lock(&sugov_lock);
		for_each_cpu(j, policy->cpus) {
			sg = &per_cpu(sugov_cpu, j);
			sg->policy = policy;
			sg->target_freq = policy->cur;
			spin_lock_init(&sg->target_freq_lock);
			init_timer_deferrable(&sg->timer);
			sg->timer.function = sugov_timer;
			sg->timer.data = j;
			sg->started = true;
			sugov_start_timer(j);
		}
		mutex_unlock(&sugov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&sugov_lock);
		for_each_cpu(j, policy->cpus) {
			sg = &per_cpu(sugov_cpu, j);
			sg->started = false;
			del_timer_sync(&sg->timer);
			sg->target_freq = 0;
		}
		mutex_unlock(&sugov_lock);
		break;

	case CPUFREQ_GOV_LIMITS:
		sg = &per_cpu(sugov_cpu, policy->cpu);
		if (!sg->started) {
			__cpufreq_driver_target(policy,
					policy->cur, CPUFREQ_RELATION_L);
			break;
		}

		mutex_lock(&sugov_lock);
		for_each_cpu(j, policy->cpus) {
			sg = &per_cpu(sugov_cpu, j);
			spin_lock_irq(&sg->target_freq_lock);
			sg->target_freq = clamp(sg->target_freq,
						policy->min, policy->max);
			spin_unlock_irq(&sg->target_freq_lock);
		}
		__cpufreq_driver_target(policy,
				policy->cur, CPUFREQ_RELATION_L);
		mutex_unlock(&sugov_lock);
		break;
	}

	return 0;
}

static struct cpufreq_governor cpufreq_gov_schedutil = {
	.name = "schedutil",
	.governor = cpufreq_governor_sugov,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static int __init sugov_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_schedutil);
}

static void __exit sugov_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_schedutil);
}

module_init(sugov_init);
module_exit(sugov_exit);

MODULE_AUTHOR("cinnamon-kernel");
MODULE_DESCRIPTION("'schedutil' - CPUFreq governor based on scheduler utilization");
MODULE_LICENSE("GPL v2");
