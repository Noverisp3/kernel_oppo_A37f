/*
 * Cinnamon_Active - Execute commands after delay
 * 
 * This module provides a delayed execution mechanism for power management
 * commands. It automatically triggers Cinnamon_Active 30 seconds after boot
 * to allow system stabilization.
 *
 * Features:
 * - 30 second delayed execution on module load
 * - Manual control via proc interface
 * - zRAM, VM, and scheduler optimizations
 *
 * Copyright (C) 2026 Cinnamon Kernel Project
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fs.h>

/* Delay period in milliseconds */
#define CINNAMON_DELAY_MS	30000	/* 30 seconds */

/* Work structure for delayed execution */
static struct delayed_work cinnamon_delay_work;
static bool delay_pending = false;
static bool auto_trigger = true;  /* Tự động chạy khi load module */
static bool auto_init_done = false;  /* Track if auto init has been attempted */
module_param(auto_trigger, bool, 0644);
MODULE_PARM_DESC(auto_trigger, "Enable auto execution after boot");

/* Proc entry */
static struct proc_dir_entry *cinnamon_delay_proc_entry;

/* Safe sysfs write function using call_usermodehelper */
static int cinnamon_write_sysfs(const char *path, const char *value)
{
	char *argv[] = {"/system/bin/sh", "-c", NULL, NULL};
	char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
	char command[256];
	int ret;

	snprintf(command, sizeof(command), "echo '%s' > '%s'", value, path);
	argv[2] = command;

	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret != 0) {
		pr_err("Cinnamon_Active: Failed: %s (ret=%d)\n", command, ret);
		return ret;
	}
	pr_info("Cinnamon_Active: Success: %s\n", command);
	return 0;
}

/* Helper function to check if file exists */
static int cinnamon_file_exists(const char *path)
{
	struct file *file;
	int exists = 0;
	
	file = filp_open(path, O_RDONLY, 0);
	if (!IS_ERR(file)) {
		exists = 1;
		filp_close(file, NULL);
	}
	return exists;
}

/* Dynamic block device detection */
static char *cinnamon_find_main_block(void)
{
	/* Check common block devices in order of preference */
	if (cinnamon_file_exists("/sys/block/mmcblk0/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: mmcblk0\n");
		return "mmcblk0";
	}
	if (cinnamon_file_exists("/sys/block/sda/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: sda\n");
		return "sda";
	}
	if (cinnamon_file_exists("/sys/block/nvme0n1/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: nvme0n1\n");
		return "nvme0n1";
	}
	if (cinnamon_file_exists("/sys/block/ufs0/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: ufs0\n");
		return "ufs0";
	}
	pr_warn("Cinnamon_Active: No suitable block device found\n");
	return NULL;
}

/* Cinnamon_Active command functions */
static void cinnamon_execute_command_1(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 1\n");
	
	/* Step 1: Cleanup old zRAM and reset */
	{
		/* Check if zram0 exists before proceeding */
		if (!cinnamon_file_exists("/sys/block/zram0/comp_algorithm")) {
			pr_err("Cinnamon_Active: zram0 not available\n");
			return;
		}
		
		/* Minimal wait for system stabilization */
		msleep(50);
		
		/* echo 1 > /sys/block/zram0/reset */
		if (cinnamon_write_sysfs("/sys/block/zram0/reset", "1") != 0) {
			pr_err("Cinnamon_Active: Failed to reset zram0\n");
		}
		
		/* Reduced wait - reset usually completes quickly */
		msleep(100);
	}
	pr_info("Cinnamon_Active: zRAM cleanup completed\n");
}

static void cinnamon_execute_command_2(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 2\n");
	
	/* Step 2: Setup Cinnamon compression and disk size */
	{
		/* Check if zram0 exists before proceeding */
		if (!cinnamon_file_exists("/sys/block/zram0/comp_algorithm")) {
			pr_err("Cinnamon_Active: zram0 not available for setup\n");
			return;
		}
		
		/* Reduced wait - previous step already completed */
		msleep(100);
		
		/* echo cinnamon > /sys/block/zram0/comp_algorithm */
		if (cinnamon_write_sysfs("/sys/block/zram0/comp_algorithm", "cinnamon") != 0) {
			pr_err("Cinnamon_Active: Failed to set comp_algorithm\n");
		}
		
		/* Minimal wait between sysfs operations */
		msleep(50);
		
		/* echo 805306368 > /sys/block/zram0/disksize */
		if (cinnamon_write_sysfs("/sys/block/zram0/disksize", "805306368") != 0) {
			pr_err("Cinnamon_Active: Failed to set disksize\n");
		}
		
		/* Reduced wait - disksize operation is fast */
		msleep(200);
		
		/* mkswap /dev/block/zram0 */
		{
			char *check_argv[] = {"/system/bin/sh", "-c", "which mkswap >/dev/null 2>&1", NULL};
			char *check_envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int check_ret = call_usermodehelper(check_argv[0], check_argv, check_envp, UMH_WAIT_PROC);
			if (check_ret != 0) {
				pr_err("Cinnamon_Active: mkswap command not found, skipping swap setup\n");
			} else {
				char *argv[] = {"/system/bin/mkswap", "/dev/block/zram0", NULL};
				char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
				int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
				if (ret != 0) {
					pr_err("Cinnamon_Active: mkswap failed with code %d\n", ret);
				} else {
					pr_info("Cinnamon_Active: mkswap completed successfully\n");
				}
			}
		}
		
		msleep(500);
		
		/* swapon /dev/block/zram0 -p 10 */
		{
			char *check_argv[] = {"/system/bin/sh", "-c", "which swapon >/dev/null 2>&1", NULL};
			char *check_envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int check_ret = call_usermodehelper(check_argv[0], check_argv, check_envp, UMH_WAIT_PROC);
			if (check_ret != 0) {
				pr_err("Cinnamon_Active: swapon command not found, skipping swap activation\n");
			} else {
				char *argv[] = {"/system/bin/swapon", "/dev/block/zram0", "-p", "10", NULL};
				char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
				int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
				if (ret != 0) {
					pr_err("Cinnamon_Active: swapon failed with code %d\n", ret);
				} else {
					pr_info("Cinnamon_Active: swapon completed successfully\n");
				}
			}
		}
	}
	pr_info("Cinnamon_Active: zRAM compression and swap setup completed\n");
}

static void cinnamon_execute_command_3(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 3\n");
	
	/* Step 3: VM optimization */
	{
		/* Minimal wait - previous step already completed */
		msleep(50);
		
		/* echo 80 > /proc/sys/vm/swappiness */
		if (cinnamon_write_sysfs("/proc/sys/vm/swappiness", "80") != 0) {
			pr_err("Cinnamon_Active: Failed to set swappiness\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 50 > /proc/sys/vm/vfs_cache_pressure */
		if (cinnamon_write_sysfs("/proc/sys/vm/vfs_cache_pressure", "50") != 0) {
			pr_err("Cinnamon_Active: Failed to set vfs_cache_pressure\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 5 > /proc/sys/vm/dirty_background_ratio */
		if (cinnamon_write_sysfs("/proc/sys/vm/dirty_background_ratio", "5") != 0) {
			pr_err("Cinnamon_Active: Failed to set dirty_background_ratio\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 15 > /proc/sys/vm/dirty_ratio */
		if (cinnamon_write_sysfs("/proc/sys/vm/dirty_ratio", "15") != 0) {
			pr_err("Cinnamon_Active: Failed to set dirty_ratio\n");
		}
	}
	
	/* Step 4: Scheduler optimization */
	{
		char *main_block;
		char scheduler_path[256];
		char readahead_path[256];
		
		main_block = cinnamon_find_main_block();
		if (!main_block) {
			pr_err("Cinnamon_Active: Failed to find main block device\n");
			return;
		}
		
		/* Minimal wait - VM operations completed */
		msleep(50);
		
		/* Setup scheduler path dynamically */
		snprintf(scheduler_path, sizeof(scheduler_path), 
			"/sys/block/%s/queue/scheduler", main_block);
		
		/* echo cinnamon > /sys/block/[device]/queue/scheduler */
		if (cinnamon_write_sysfs(scheduler_path, "cinnamon") != 0) {
			pr_err("Cinnamon_Active: Failed to set scheduler\n");
		}
		
		/* Minimal wait between scheduler operations */
		msleep(25);
		
		/* Setup read_ahead path dynamically */
		snprintf(readahead_path, sizeof(readahead_path), 
			"/sys/block/%s/queue/read_ahead_kb", main_block);
		
		/* echo 512 > /sys/block/[device]/queue/read_ahead_kb */
		if (cinnamon_write_sysfs(readahead_path, "512") != 0) {
			pr_err("Cinnamon_Active: Failed to set read_ahead_kb\n");
		}
	}
	
	/* Step 5: USB Gadget setup for ADB+HID */
	{
		msleep(100);  /* Small wait before USB operations */
		
		/* Tắt gadget */
		if (cinnamon_write_sysfs("/sys/class/android_usb/android0/enable", "0") != 0) {
			pr_err("Cinnamon_Active: Failed to disable gadget\n");
		}
		
		/* Reduced wait - USB disable is usually fast */
		msleep(500);  /* Reduced from 1000ms */
		
		/* Set functions: ADB (ffs) và HID (hid) */
		if (cinnamon_write_sysfs("/sys/class/android_usb/android0/functions", "ffs,hid") != 0) {
			pr_err("Cinnamon_Active: Failed to set functions\n");
		}
		
		/* Reduced wait - functions setup is quick */
		msleep(300);  /* Reduced from 500ms */
		
		/* Bật lại gadget */
		if (cinnamon_write_sysfs("/sys/class/android_usb/android0/enable", "1") != 0) {
			pr_err("Cinnamon_Active: Failed to enable gadget\n");
		}
		
		/* Reduced wait - USB enumeration is faster than expected */
		msleep(500);  /* Reduced from 1000ms */
	}
	pr_info("Cinnamon_Active: VM, scheduler, and USB gadget optimization completed\n");
}

/* Main work function - executes after delay */
static void cinnamon_delay_work_fn(struct work_struct *work)
{
	pr_info("Cinnamon_Active: Auto-executing Cinnamon_Active after %d ms\n", 
		CINNAMON_DELAY_MS);
	
	delay_pending = false;
	
	pr_info("Cinnamon_Active: Executing Step 1\n");
	cinnamon_execute_command_1();
	
	pr_info("Cinnamon_Active: Executing Step 2\n");
	cinnamon_execute_command_2();
	
	pr_info("Cinnamon_Active: Executing Step 3\n");
	cinnamon_execute_command_3();
	
	pr_info("Cinnamon_Active: Auto execution completed\n");
}

/* Start delayed execution */
static void cinnamon_start_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon_Active: Delay already in progress, cancelling previous\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	}
	
	pr_info("Cinnamon_Active: Starting %d second delay\n", 
		CINNAMON_DELAY_MS / 1000);
	
	delay_pending = true;
	schedule_delayed_work(&cinnamon_delay_work, msecs_to_jiffies(CINNAMON_DELAY_MS));
}

/* Cancel delayed execution */
static void cinnamon_cancel_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon_Active: Cancelling delayed execution\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	} else {
		pr_info("Cinnamon_Active: No delay in progress\n");
	}
}

/* Proc interface - show status */
static int cinnamon_delay_show(struct seq_file *m, void *v)
{
	/* Lazy auto initialization - trigger when proc file is first read */
	if (auto_trigger && !auto_init_done) {
		pr_info("Cinnamon_Active: Lazy auto-trigger activated on first proc read\n");
		auto_init_done = true;

		if (!delay_pending) {
			pr_info("Cinnamon_Active: Scheduling delayed work in %d seconds\n",
				CINNAMON_DELAY_MS / 1000);
			delay_pending = true;
			schedule_delayed_work(&cinnamon_delay_work, msecs_to_jiffies(CINNAMON_DELAY_MS));
			pr_info("Cinnamon_Active: Auto-trigger work scheduled successfully\n");
		} else {
			pr_info("Cinnamon_Active: Work already pending, skipping auto trigger\n");
		}
	}

	seq_printf(m, "Cinnamon_Active Status:\n");
	seq_printf(m, "Delay Period: %d seconds\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "Delay Pending: %s\n", delay_pending ? "YES" : "NO");
	seq_printf(m, "Workqueue Status: %s\n", delay_pending ? "SCHEDULED" : "IDLE");
	seq_printf(m, "Auto Trigger on Load: %s\n", auto_trigger ? "ENABLED" : "DISABLED");
	seq_printf(m, "Auto Init Done: %s\n", auto_init_done ? "YES" : "NO");
	seq_printf(m, "\nCinnamon_Active Commands:\n");
	seq_printf(m, "1 - Start Cinnamon_Active (%d second delay)\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "2 - Cancel Cinnamon_Active\n");
	seq_printf(m, "3 - Force immediate Cinnamon_Active\n");
	seq_printf(m, "4 - Disable auto trigger on next boot (temporary)\n");
	seq_printf(m, "5 - Enable auto trigger\n");
	return 0;
}

/* Proc interface - write control */
static ssize_t cinnamon_delay_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[16];
	int val;
	
	if (count > sizeof(kbuf) - 1)
		return -EINVAL;
		
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
		
	kbuf[count] = '\0';
	if (sscanf(kbuf, "%d", &val) != 1)
		return -EINVAL;
		
	switch (val) {
	case 1:
		pr_info("Cinnamon_Active: Manual start with delay\n");
		cinnamon_start_delay();
		break;
	case 2:
		pr_info("Cinnamon_Active: Manual cancel\n");
		cinnamon_cancel_delay();
		break;
	case 3:
		pr_info("Cinnamon_Active: Force immediate execution\n");
		if (delay_pending) {
			cancel_delayed_work_sync(&cinnamon_delay_work);
			delay_pending = false;
		}
		/* Execute immediately */
		cinnamon_delay_work_fn(NULL);
		break;
	case 4:
		pr_info("Cinnamon_Active: Disable auto trigger for this boot\n");
		auto_trigger = false;
		break;
	case 5:
		pr_info("Cinnamon_Active: Enable auto trigger\n");
		auto_trigger = true;
		break;
	default:
		return -EINVAL;
	}
	
	return count;
}

/* Proc file operations */
static int cinnamon_delay_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cinnamon_delay_show, NULL);
}

static const struct file_operations cinnamon_delay_fops = {
	.owner		= THIS_MODULE,
	.open		= cinnamon_delay_proc_open,
	.read		= seq_read,
	.write		= cinnamon_delay_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* Module initialization */
static int __init cinnamon_delay_trigger_init(void)
{
	pr_info("Cinnamon_Active: Module init STARTING\n");
	
	INIT_DELAYED_WORK(&cinnamon_delay_work, cinnamon_delay_work_fn);
	
	cinnamon_delay_proc_entry = proc_create_data("cinnamon_delay_trigger", 0666, NULL,
						&cinnamon_delay_fops, NULL);
	if (!cinnamon_delay_proc_entry) {
		pr_err("Cinnamon_Active: Failed to create proc entry\n");
		return -ENOMEM;
	}
	
	pr_info("Cinnamon_Active: auto_trigger parameter = %d\n", auto_trigger);
	pr_info("Cinnamon_Active: Module init COMPLETE\n");
	
	/* Tự động schedule work sau 30 giây nếu auto_trigger được bật */
	if (auto_trigger) {
		pr_info("Cinnamon_Active: Auto-trigger enabled, waiting for system stabilization\n");
		
		/* Wait for system to stabilize before scheduling work */
		msleep(2000); /* 2 second delay for workqueue to be ready */
		
		pr_info("Cinnamon_Active: Scheduling delayed work in %d seconds\n",
			CINNAMON_DELAY_MS / 1000);
		
		if (delay_pending) {
			pr_info("Cinnamon_Active: Delay already pending, cancelling previous\n");
			cancel_delayed_work_sync(&cinnamon_delay_work);
			delay_pending = false;
		}
		delay_pending = true;
		schedule_delayed_work(&cinnamon_delay_work, msecs_to_jiffies(CINNAMON_DELAY_MS));
		pr_info("Cinnamon_Active: Auto-trigger work scheduled successfully\n");
	} else {
		pr_info("Cinnamon_Active: Auto-trigger disabled, waiting for manual command\n");
	}
	
	pr_info("Cinnamon_Active: Module ready\n");
	return 0;
}

/* Module cleanup */
static void __exit cinnamon_delay_trigger_exit(void)
{
	pr_info("Cinnamon_Active: Shutting down\n");
	
	if (delay_pending) {
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	}
	
	if (cinnamon_delay_proc_entry)
		proc_remove(cinnamon_delay_proc_entry);
}

subsys_initcall(cinnamon_delay_trigger_init);
module_exit(cinnamon_delay_trigger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Noveris");
MODULE_DESCRIPTION("Cinnamon_Active");
MODULE_VERSION("2.1");