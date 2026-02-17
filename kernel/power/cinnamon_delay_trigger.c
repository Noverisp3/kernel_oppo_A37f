/*
 * Cinnamon Delay Trigger - Execute commands after delay
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

/* Proc entry */
static struct proc_dir_entry *cinnamon_delay_proc_entry;

/* Cinnamon_Active command functions */
static void cinnamon_execute_command_1(void)
{
	pr_info("Cinnamon Delay Trigger: Executing Cinnamon_Active - Step 1\n");
	
	/* Step 1: Cleanup old zRAM and reset */
	{
		struct file *file;
		char *buf = "1";
		mm_segment_t old_fs;
		loff_t pos = 0;
		
		/* Wait for system to stabilize */
		msleep(100);
		
		/* echo 1 > /sys/block/zram0/reset */
		file = filp_open("/sys/block/zram0/reset", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open /sys/block/zram0/reset: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write to /sys/block/zram0/reset\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully reset zram0\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		/* Wait for reset to complete */
		msleep(200);
	}
	pr_info("Cinnamon Delay Trigger: zRAM cleanup completed\n");
}

static void cinnamon_execute_command_2(void)
{
	pr_info("Cinnamon Delay Trigger: Executing Cinnamon_Active - Step 2\n");
	
	/* Step 2: Setup Cinnamon compression and disk size */
	{
		struct file *file;
		char *buf;
		mm_segment_t old_fs;
		loff_t pos = 0;
		
		/* Wait for previous step to complete */
		msleep(300);
		
		/* echo cinnamon > /sys/block/zram0/comp_algorithm */
		buf = "cinnamon";
		file = filp_open("/sys/block/zram0/comp_algorithm", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open comp_algorithm: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write comp_algorithm\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set comp_algorithm to cinnamon\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(100);
		
		/* echo 805306368 > /sys/block/zram0/disksize */
		buf = "805306368";
		file = filp_open("/sys/block/zram0/disksize", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open disksize: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write disksize\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set disksize to 805306368\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(200);
		
		/* mkswap /dev/block/zram0 */
		{
			char *argv[] = {"/system/bin/mkswap", "/dev/block/zram0", NULL};
			char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
			if (ret != 0) {
				pr_err("Cinnamon Delay Trigger: mkswap failed with code %d\n", ret);
			} else {
				pr_info("Cinnamon Delay Trigger: mkswap completed successfully\n");
			}
		}
		
		msleep(500);
		
		/* swapon /dev/block/zram0 -p 10 */
		{
			char *argv[] = {"/system/bin/swapon", "/dev/block/zram0", "-p", "10", NULL};
			char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
			if (ret != 0) {
				pr_err("Cinnamon Delay Trigger: swapon failed with code %d\n", ret);
			} else {
				pr_info("Cinnamon Delay Trigger: swapon completed successfully\n");
			}
		}
	}
	pr_info("Cinnamon Delay Trigger: zRAM compression and swap setup completed\n");
}

static void cinnamon_execute_command_3(void)
{
	pr_info("Cinnamon Delay Trigger: Executing Cinnamon_Active - Step 3\n");
	
	/* Step 3: VM optimization */
	{
		struct file *file;
		char *buf;
		mm_segment_t old_fs;
		loff_t pos = 0;
		
		/* Wait for previous step to complete */
		msleep(300);
		
		/* echo 100 > /proc/sys/vm/swappiness */
		buf = "100";
		file = filp_open("/proc/sys/vm/swappiness", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open swappiness: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write swappiness\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set swappiness to 100\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(50);
		
		/* echo 50 > /proc/sys/vm/vfs_cache_pressure */
		buf = "50";
		file = filp_open("/proc/sys/vm/vfs_cache_pressure", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open vfs_cache_pressure: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write vfs_cache_pressure\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set vfs_cache_pressure to 50\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(50);
		
		/* echo 5 > /proc/sys/vm/dirty_background_ratio */
		buf = "5";
		file = filp_open("/proc/sys/vm/dirty_background_ratio", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open dirty_background_ratio: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write dirty_background_ratio\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set dirty_background_ratio to 5\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(50);
		
		/* echo 15 > /proc/sys/vm/dirty_ratio */
		buf = "15";
		file = filp_open("/proc/sys/vm/dirty_ratio", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open dirty_ratio: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write dirty_ratio\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set dirty_ratio to 15\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
	}
	
	/* Step 4: Scheduler optimization - khai báo lại biến trong block này */
	{
		struct file *file;
		char *buf;
		mm_segment_t old_fs;
		loff_t pos = 0;
		
		msleep(100);
		
		/* echo cinnamon > /sys/block/mmcblk0/queue/scheduler */
		buf = "cinnamon";
		file = filp_open("/sys/block/mmcblk0/queue/scheduler", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open scheduler: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write scheduler\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set scheduler to cinnamon\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
		
		msleep(50);
		
		/* echo 512 > /sys/block/mmcblk0/queue/read_ahead_kb */
		buf = "512";
		file = filp_open("/sys/block/mmcblk0/queue/read_ahead_kb", O_WRONLY, 0);
		if (IS_ERR(file)) {
			pr_err("Cinnamon Delay Trigger: Failed to open read_ahead_kb: %ld\n", PTR_ERR(file));
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (vfs_write(file, buf, strlen(buf), &pos) < 0) {
				pr_err("Cinnamon Delay Trigger: Failed to write read_ahead_kb\n");
			} else {
				pr_info("Cinnamon Delay Trigger: Successfully set read_ahead_kb to 512\n");
			}
			set_fs(old_fs);
			filp_close(file, NULL);
		}
	}
	pr_info("Cinnamon Delay Trigger: VM and scheduler optimization completed\n");
}

/* Main work function - executes after delay */
static void cinnamon_delay_work_fn(struct work_struct *work)
{
	pr_info("Cinnamon Delay Trigger: Auto-executing Cinnamon_Active after %d ms\n", 
		CINNAMON_DELAY_MS);
	
	delay_pending = false;
	
	pr_info("Cinnamon Delay Trigger: Executing Step 1 (zRAM cleanup)\n");
	cinnamon_execute_command_1();
	
	pr_info("Cinnamon Delay Trigger: Executing Step 2 (compression & swap)\n");
	cinnamon_execute_command_2();
	
	pr_info("Cinnamon Delay Trigger: Executing Step 3 (VM & scheduler)\n");
	cinnamon_execute_command_3();
	
	pr_info("Cinnamon Delay Trigger: Auto execution completed\n");
}

/* Start delayed execution */
static void cinnamon_start_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon Delay Trigger: Delay already in progress, cancelling previous\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
	}
	
	pr_info("Cinnamon Delay Trigger: Starting %d second delay\n", 
		CINNAMON_DELAY_MS / 1000);
	
	delay_pending = true;
	schedule_delayed_work(&cinnamon_delay_work, msecs_to_jiffies(CINNAMON_DELAY_MS));
}

/* Cancel delayed execution */
static void cinnamon_cancel_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon Delay Trigger: Cancelling delayed execution\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	} else {
		pr_info("Cinnamon Delay Trigger: No delay in progress\n");
	}
}

/* Proc interface - show status */
static int cinnamon_delay_show(struct seq_file *m, void *v)
{
	seq_printf(m, "=== Cinnamon Delay Trigger Status ===\n");
	seq_printf(m, "Delay Period: %d seconds\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "Delay Pending: %s\n", delay_pending ? "YES" : "NO");
	seq_printf(m, "Workqueue Status: %s\n", delay_pending ? "SCHEDULED" : "IDLE");
	seq_printf(m, "Auto Trigger on Load: %s\n", auto_trigger ? "ENABLED" : "DISABLED");
	seq_printf(m, "\n=== Cinnamon_Active Commands ===\n");
	seq_printf(m, "1 - Start Cinnamon_Active (%d second delay)\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "2 - Cancel Cinnamon_Active\n");
	seq_printf(m, "3 - Force immediate Cinnamon_Active\n");
	seq_printf(m, "4 - Disable auto trigger on next boot (temporary)\n");
	seq_printf(m, "5 - Enable auto trigger\n");
	seq_printf(m, "\n=== Command Sequence ===\n");
	seq_printf(m, "Step 1: zRAM cleanup and reset\n");
	seq_printf(m, "Step 2: Cinnamon compression and swap setup\n");
	seq_printf(m, "Step 3: VM and scheduler optimization\n");
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
		pr_info("Cinnamon Delay Trigger: Manual start with delay\n");
		cinnamon_start_delay();
		break;
	case 2:
		pr_info("Cinnamon Delay Trigger: Manual cancel\n");
		cinnamon_cancel_delay();
		break;
	case 3:
		pr_info("Cinnamon Delay Trigger: Force immediate execution\n");
		if (delay_pending) {
			cancel_delayed_work_sync(&cinnamon_delay_work);
			delay_pending = false;
		}
		/* Execute immediately */
		cinnamon_delay_work_fn(NULL);
		break;
	case 4:
		pr_info("Cinnamon Delay Trigger: Disable auto trigger for this boot\n");
		auto_trigger = false;
		break;
	case 5:
		pr_info("Cinnamon Delay Trigger: Enable auto trigger\n");
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
	pr_info("Cinnamon Delay Trigger: Initializing\n");
	
	INIT_DELAYED_WORK(&cinnamon_delay_work, cinnamon_delay_work_fn);
	
	cinnamon_delay_proc_entry = proc_create("cinnamon_delay_trigger", 0, NULL,
						&cinnamon_delay_fops);
	if (!cinnamon_delay_proc_entry) {
		pr_err("Cinnamon Delay Trigger: Failed to create proc entry\n");
		return -ENOMEM;
	}
	
	/* Tự động schedule work sau 30 giây nếu auto_trigger được bật */
	if (auto_trigger) {
		pr_info("Cinnamon Delay Trigger: Auto-trigger enabled, scheduling in %d seconds\n",
			CINNAMON_DELAY_MS / 1000);
		delay_pending = true;
		schedule_delayed_work(&cinnamon_delay_work, msecs_to_jiffies(CINNAMON_DELAY_MS));
	} else {
		pr_info("Cinnamon Delay Trigger: Auto-trigger disabled, waiting for manual command\n");
	}
	
	pr_info("Cinnamon Delay Trigger: Ready\n");
	return 0;
}

/* Module cleanup */
static void __exit cinnamon_delay_trigger_exit(void)
{
	pr_info("Cinnamon Delay Trigger: Shutting down\n");
	
	if (delay_pending) {
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	}
	
	if (cinnamon_delay_proc_entry)
		proc_remove(cinnamon_delay_proc_entry);
}

module_init(cinnamon_delay_trigger_init);
module_exit(cinnamon_delay_trigger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Noveris");
MODULE_DESCRIPTION("Cinnamon Delay Trigger - Automatic Cinnamon_Active 30s after boot");
MODULE_VERSION("2.0");