/*
 * SuperPageMerge (SPM) sysfs interface
 */

#include <linux/ksm.h>
#include <linux/spm.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>

int spm_enabled = 0;

/* ----------------------------------------------------------------------
 * Attribute show/store functions
 * ---------------------------------------------------------------------- */
static ssize_t spm_enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_enabled);
}
static ssize_t spm_enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int enable;
	if (sscanf(buf, "%d", &enable) != 1)
		return -EINVAL;
	if (enable) {
		if (!spm_scanner_thread)
			spm_start_scanner();
	} else {
		if (spm_scanner_thread)
			spm_stop_scanner();
	}
	spm_enabled = enable ? 1 : 0;
	return count;
}

static ssize_t spm_run_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_scanner_thread ? 1 : 0);
}
static ssize_t spm_run_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int run;
	if (sscanf(buf, "%d", &run) != 1)
		return -EINVAL;
	if (run) {
		if (!spm_scanner_thread)
			spm_start_scanner();
	} else {
		if (spm_scanner_thread)
			spm_stop_scanner();
	}
	return count;
}

static ssize_t spm_pages_merged_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_pages_merged));
}
static ssize_t spm_pages_saved_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_pages_saved));
}
static ssize_t spm_scan_cycles_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_scan_cycles));
}

static ssize_t spm_scan_period_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.scan_period_ms);
}
static ssize_t spm_scan_period_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int period;
	if (sscanf(buf, "%d", &period) != 1 || period < 10 || period > 10000)
		return -EINVAL;
	spm_cfg.scan_period_ms = period;
	return count;
}

static ssize_t spm_merge_threshold_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.merge_threshold);
}
static ssize_t spm_merge_threshold_store(struct kobject *kobj, struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int threshold;
	if (sscanf(buf, "%d", &threshold) != 1 || threshold < 50 || threshold > 100)
		return -EINVAL;
	spm_cfg.merge_threshold = threshold;
	return count;
}

static ssize_t spm_enable_fork_sharing_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.enable_fork_sharing ? 1 : 0);
}
static ssize_t spm_enable_fork_sharing_store(struct kobject *kobj, struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int enable;
	if (sscanf(buf, "%d", &enable) != 1)
		return -EINVAL;
	spm_cfg.enable_fork_sharing = enable ? true : false;
	return count;
}

static ssize_t spm_scan_all_vmas_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_scan_all_vmas);
}
static ssize_t spm_scan_all_vmas_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) != 1 || (val != 0 && val != 1))
		return -EINVAL;
	spm_scan_all_vmas = val;
	return count;
}

static ssize_t spm_scan_limit_pages_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", spm_cfg.max_pages_per_scan);
}
static ssize_t spm_scan_limit_pages_store(struct kobject *kobj, struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned long limit;
	if (sscanf(buf, "%lu", &limit) != 1)
		return -EINVAL;
	spm_cfg.max_pages_per_scan = limit;
	if (limit)
		spm_max_pages_per_cycle = limit;
	else
		spm_update_scan_limit();
	return count;
}

static ssize_t spm_adaptive_rate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.adaptive_rate ? 1 : 0);
}
static ssize_t spm_adaptive_rate_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int enable;
	if (sscanf(buf, "%d", &enable) != 1 || (enable != 0 && enable != 1))
		return -EINVAL;
	spm_cfg.adaptive_rate = enable ? true : false;
	return count;
}

static ssize_t spm_hot_threshold_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", spm_cfg.hot_threshold);
}
static ssize_t spm_hot_threshold_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int threshold;
	if (sscanf(buf, "%u", &threshold) != 1)
		return -EINVAL;
	spm_cfg.hot_threshold = threshold;
	return count;
}

static ssize_t spm_register_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "Write 1 to register current process\n");
}

static ssize_t spm_register_store(struct kobject *kobj, struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int val;
	struct mm_struct *mm;

	if (sscanf(buf, "%d", &val) != 1 || val != 1)
		return -EINVAL;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	spm_enter(mm);
	return count;
}

/* ----------------------------------------------------------------------
 * Attribute definitions
 * ---------------------------------------------------------------------- */
static struct kobj_attribute spm_run_attr = __ATTR(run, 0644, spm_run_show, spm_run_store);
static struct kobj_attribute spm_pages_merged_attr = __ATTR(pages_merged, 0444, spm_pages_merged_show, NULL);
static struct kobj_attribute spm_pages_saved_attr = __ATTR(pages_saved, 0444, spm_pages_saved_show, NULL);
static struct kobj_attribute spm_scan_cycles_attr = __ATTR(scan_cycles, 0444, spm_scan_cycles_show, NULL);
static struct kobj_attribute spm_scan_period_ms_attr = __ATTR(scan_period_ms, 0644,
							      spm_scan_period_ms_show, spm_scan_period_ms_store);
static struct kobj_attribute spm_merge_threshold_attr = __ATTR(merge_threshold, 0644,
							       spm_merge_threshold_show, spm_merge_threshold_store);
static struct kobj_attribute spm_enable_fork_sharing_attr = __ATTR(enable_fork_sharing, 0644,
								   spm_enable_fork_sharing_show, spm_enable_fork_sharing_store);
static struct kobj_attribute spm_enabled_attr = __ATTR(enabled, 0644, spm_enabled_show, spm_enabled_store);
static struct kobj_attribute spm_scan_all_vmas_attr = __ATTR(scan_all_vmas, 0644,
							     spm_scan_all_vmas_show, spm_scan_all_vmas_store);
static struct kobj_attribute spm_scan_limit_pages_attr = __ATTR(scan_limit_pages, 0644,
								spm_scan_limit_pages_show, spm_scan_limit_pages_store);
static struct kobj_attribute spm_adaptive_rate_attr = __ATTR(adaptive_rate, 0644,
							     spm_adaptive_rate_show, spm_adaptive_rate_store);
static struct kobj_attribute spm_hot_threshold_attr = __ATTR(hot_threshold, 0644,
							     spm_hot_threshold_show, spm_hot_threshold_store);
static struct kobj_attribute spm_register_attr = __ATTR(register, 0644,
						       spm_register_show, spm_register_store);

static struct attribute *spm_attrs[] = {
	&spm_run_attr.attr,
	&spm_pages_merged_attr.attr,
	&spm_pages_saved_attr.attr,
	&spm_scan_cycles_attr.attr,
	&spm_scan_period_ms_attr.attr,
	&spm_merge_threshold_attr.attr,
	&spm_enable_fork_sharing_attr.attr,
	&spm_enabled_attr.attr,
	&spm_scan_all_vmas_attr.attr,
	&spm_scan_limit_pages_attr.attr,
	&spm_adaptive_rate_attr.attr,
	&spm_hot_threshold_attr.attr,
	&spm_register_attr.attr,
	NULL,
};

static struct attribute_group spm_attr_group = {
	.attrs = spm_attrs,
};

/* ----------------------------------------------------------------------
 * Init / cleanup
 * ---------------------------------------------------------------------- */
int spm_sysfs_init(void)
{
	struct kobject *spm_kobj;
	int ret;

	spm_kobj = kobject_create_and_add("spm", kernel_kobj);
	if (!spm_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(spm_kobj, &spm_attr_group);
	if (ret) {
		kobject_put(spm_kobj);
		return ret;
	}
	return 0;
}

void spm_sysfs_cleanup(void)
{
	/* sysfs kobject will be cleaned up automatically on module exit */
}