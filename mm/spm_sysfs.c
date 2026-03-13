/*
 * SuperPageMerge (SPM) sysfs interface
 * 
 * Configuration and monitoring interface for SPM
 */

#include <linux/ksm.h>
#include <linux/spm.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>

int spm_enabled = 0;

/* SPM sysfs attributes */
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
static ssize_t spm_run_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
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

static ssize_t spm_pages_merged_show(struct kobject *kobj, 
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_pages_merged));
}

static ssize_t spm_pages_saved_show(struct kobject *kobj, 
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_pages_saved));
}

static ssize_t spm_scan_cycles_show(struct kobject *kobj, 
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", atomic_long_read(&spm_scan_cycles));
}

static ssize_t spm_scan_period_ms_show(struct kobject *kobj, 
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.scan_period_ms);
}

static ssize_t spm_scan_period_ms_store(struct kobject *kobj, 
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int period;
	
	if (sscanf(buf, "%d", &period) != 1 || period < 10 || period > 10000)
		return -EINVAL;
	
	spm_cfg.scan_period_ms = period;
	return count;
}

static ssize_t spm_merge_threshold_show(struct kobject *kobj, 
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.merge_threshold);
}

static ssize_t spm_merge_threshold_store(struct kobject *kobj, 
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int threshold;
	
	if (sscanf(buf, "%d", &threshold) != 1 || threshold < 50 || threshold > 100)
		return -EINVAL;
	
	spm_cfg.merge_threshold = threshold;
	return count;
}

static ssize_t spm_enable_fork_sharing_show(struct kobject *kobj, 
					    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spm_cfg.enable_fork_sharing ? 1 : 0);
}

static ssize_t spm_enable_fork_sharing_store(struct kobject *kobj, 
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int enable;
	
	if (sscanf(buf, "%d", &enable) != 1)
		return -EINVAL;
	
	spm_cfg.enable_fork_sharing = enable ? true : false;
	return count;
}

/* Define sysfs attributes */
static struct kobj_attribute spm_run_attr = __ATTR(run, 0644,
						   spm_run_show, spm_run_store);
static struct kobj_attribute spm_pages_merged_attr = __ATTR(pages_merged, 0444,
							    spm_pages_merged_show, NULL);
static struct kobj_attribute spm_pages_saved_attr = __ATTR(pages_saved, 0444,
							 spm_pages_saved_show, NULL);
static struct kobj_attribute spm_scan_cycles_attr = __ATTR(scan_cycles, 0444,
							 spm_scan_cycles_show, NULL);
static struct kobj_attribute spm_scan_period_ms_attr = __ATTR(scan_period_ms, 0644,
							     spm_scan_period_ms_show, 
							     spm_scan_period_ms_store);
static struct kobj_attribute spm_merge_threshold_attr = __ATTR(merge_threshold, 0644,
							       spm_merge_threshold_show,
							       spm_merge_threshold_store);
static struct kobj_attribute spm_enable_fork_sharing_attr = __ATTR(enable_fork_sharing, 0644,
								    spm_enable_fork_sharing_show,
								    spm_enable_fork_sharing_store);
static struct kobj_attribute spm_enabled_attr = __ATTR(enabled, 0644,
						       spm_enabled_show, spm_enabled_store);

static struct attribute *spm_attrs[] = {
	&spm_run_attr.attr,
	&spm_pages_merged_attr.attr,
	&spm_pages_saved_attr.attr,
	&spm_scan_cycles_attr.attr,
	&spm_scan_period_ms_attr.attr,
	&spm_merge_threshold_attr.attr,
	&spm_enable_fork_sharing_attr.attr,
	&spm_enabled_attr.attr,
	NULL,
};

static struct attribute_group spm_attr_group = {
	.attrs = spm_attrs,
};

/* Initialize SPM sysfs */
int spm_sysfs_init(void)
{
	struct kobject *spm_kobj;
	int ret;
	
	/* Create spm kobject under kernel */
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

/* Cleanup SPM sysfs */
void spm_sysfs_cleanup(void)
{
	/* The sysfs kobject will be automatically cleaned up on module exit */
	/* No additional cleanup needed for now */
}