/* This file is a Prototype and has been disabled (can re-enable if needed but not work yet) */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/msm_mdp.h>
#include "../video/msm/mdss/mdss_mdp.h"

extern struct fb_info *registered_fb[FB_MAX];
extern int num_registered_fb;

struct ccolorcal_data {
    int enable;
    int red;
    int green;
    int blue;
    int saturation;
    int value;
    int contrast;
    int hue;
    int invert;
    struct mutex lock;
};

struct ccolorcal_state {
    int enable;
    int red, green, blue;
    int saturation;
    int value;
    int contrast;
    int hue;
    int invert;
};

static struct ccolorcal_data ccolorcal = {
    .enable = 0,
    .red = 256,
    .green = 256,
    .blue = 256,
    .saturation = 256,
    .value = 256,
    .contrast = 256,
    .hue = 0,
    .invert = 0,
};

static struct fb_info *saved_fb_info;

static struct platform_device *ccolorcal_pdev;

DEFINE_MUTEX(state_lock);

static int ccolorcal_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);

static struct notifier_block ccolorcal_fb_notif = {
    .notifier_call = ccolorcal_fb_notifier_callback,
};

static int ccolorcal_update_display(void) {
    struct msmfb_mdp_pp *safe_pp;
    struct fb_info *info = NULL;
    uint32_t rr, gg, bb;
    struct mdss_data_type *mdata;

    // Check if enabled - return silently if disabled
    if (!ccolorcal.enable) {
        return 0;
    }

    pr_info("CColorCal: update_display called\n");

    // Lấy thông tin fb_info an toàn
    if (num_registered_fb > 0) info = registered_fb[0];
    else if (saved_fb_info) info = saved_fb_info;

    pr_info("CColorCal: info=%p, saved_fb_info=%p, num_registered_fb=%d\n", info, saved_fb_info, num_registered_fb);

    if (!info || !info->fbops || !info->fbops->fb_ioctl) {
        pr_info("CColorCal: fbops check failed\n");
        return 0;
    }

    // Check DSPP availability
    mdata = mdss_mdp_get_mdata();
    if (mdata) {
        pr_info("CColorCal: mdata=%p, ndspp=%d, nmixers_intf=%d\n", 
                mdata, mdata->ndspp, mdata->nmixers_intf);
    } else {
        pr_info("CColorCal: mdata is NULL\n");
    }

    safe_pp = kzalloc(sizeof(struct msmfb_mdp_pp), GFP_KERNEL);
    if (!safe_pp) {
        pr_info("CColorCal: failed to alloc safe_pp\n");
        return -ENOMEM;
    }

    // Try DMA_P PCC instead of DSPP for MSM8916
    pr_info("CColorCal: trying DMA_P PCC approach\n");
    safe_pp->op = mdp_op_pcc_cfg; 
    safe_pp->data.pcc_cfg_data.block = MDP_BLOCK_DMA_P;
    safe_pp->data.pcc_cfg_data.ops = MDP_PP_OPS_ENABLE | MDP_PP_OPS_WRITE;

    // Tính toán Gain từ sysfs
    rr = (ccolorcal.red * 32768) / 256;
    gg = (ccolorcal.green * 32768) / 256;
    bb = (ccolorcal.blue * 32768) / 256;

    pr_info("CColorCal: gains rr=%u gg=%u bb=%u\n", rr, gg, bb);

    // Gán vào cấu trúc chuẩn từ header - dùng r.r, g.g, b.b fields
    safe_pp->data.pcc_cfg_data.r.r = rr;
    safe_pp->data.pcc_cfg_data.g.g = gg;
    safe_pp->data.pcc_cfg_data.b.b = bb;

    // Gọi IOCTL với mã lệnh từ header
    pr_info("CColorCal: calling ioctl MSMFB_MDP_PP (DMA_P)\n");
    info->fbops->fb_ioctl(info, MSMFB_MDP_PP, (unsigned long)safe_pp);
    pr_info("CColorCal: ioctl done\n");

    // Trigger commit để enable DMA_P PCC
    pr_info("CColorCal: calling commit to enable DMA_P\n");
    info->fbops->fb_ioctl(info, MSMFB_OVERLAY_COMMIT, 0);
    pr_info("CColorCal: commit done\n");

    // Also try DSPP as backup
    pr_info("CColorCal: trying DSPP PCC as backup\n");
    safe_pp->data.pcc_cfg_data.block = MDP_LOGICAL_BLOCK_DISP_0;
    safe_pp->data.pcc_cfg_data.ops = MDP_PP_OPS_ENABLE | MDP_PP_OPS_WRITE;
    safe_pp->data.pcc_cfg_data.r.r = rr;
    safe_pp->data.pcc_cfg_data.g.g = gg;
    safe_pp->data.pcc_cfg_data.b.b = bb;
    
    info->fbops->fb_ioctl(info, MSMFB_MDP_PP, (unsigned long)safe_pp);
    info->fbops->fb_ioctl(info, MSMFB_OVERLAY_COMMIT, 0);
    pr_info("CColorCal: DSPP backup done\n");

    kfree(safe_pp);
    return 0;
}

static int ccolorcal_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data) {
    struct fb_event *evdata = data;
    struct fb_info *info = evdata->info;

    if (event == FB_EVENT_BLANK) {
        int *blank = evdata->data;
        if (*blank == FB_BLANK_UNBLANK) {
            if (info) {
                saved_fb_info = info;
                // Check if enabled before updating
                if (ccolorcal.enable) {
                    pr_info("CColorCal: Screen ON - Updating display\n");
                    ccolorcal_update_display();
                }
            }
        }
    }
    return 0;
}

#define CCOLORCAL_ATTR(_name, _mode, _show, _store) \
static struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)

static ssize_t kcal_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d %d %d\n", ccolorcal.red, ccolorcal.green, ccolorcal.blue);
}

static ssize_t kcal_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int r, g, b;
    
    // Check if enabled - return silently if disabled
    if (!ccolorcal.enable) {
        return count;
    }
    
    pr_info("CColorCal: kcal_store called with buf=%s\n", buf);
    
    sscanf(buf, "%d %d %d", &r, &g, &b);
    r = clamp(r, 1, 256);
    g = clamp(g, 1, 256);
    b = clamp(b, 1, 256);
    
    pr_info("CColorCal: setting RGB=%d %d %d\n", r, g, b);
    
    mutex_lock(&ccolorcal.lock);
    ccolorcal.red = r;
    ccolorcal.green = g;
    ccolorcal.blue = b;
    
    pr_info("CColorCal: calling update_display from kcal_store\n");
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    
    pr_info("CColorCal: kcal_store done\n");
    return count;
}

CCOLORCAL_ATTR(kcal, 0644, kcal_show, kcal_store);

static ssize_t kcal_sat_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.saturation);
}

static ssize_t kcal_sat_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, 0, 256);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.saturation = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_sat, 0644, kcal_sat_show, kcal_sat_store);

static ssize_t kcal_val_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.value);
}

static ssize_t kcal_val_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, 0, 256);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.value = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_val, 0644, kcal_val_show, kcal_val_store);

static ssize_t kcal_cont_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.contrast);
}

static ssize_t kcal_cont_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, 0, 256);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.contrast = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_cont, 0644, kcal_cont_show, kcal_cont_store);

static ssize_t kcal_hue_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.hue);
}

static ssize_t kcal_hue_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, -180, 180);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.hue = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_hue, 0644, kcal_hue_show, kcal_hue_store);

static ssize_t kcal_enable_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.enable);
}

static ssize_t kcal_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, 0, 1);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.enable = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_enable, 0644, kcal_enable_show, kcal_enable_store);

static ssize_t kcal_invert_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", ccolorcal.invert);
}

static ssize_t kcal_invert_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int val;
    sscanf(buf, "%d", &val);
    val = clamp(val, 0, 1);
    mutex_lock(&ccolorcal.lock);
    ccolorcal.invert = val;
    ccolorcal_update_display();
    mutex_unlock(&ccolorcal.lock);
    return count;
}

CCOLORCAL_ATTR(kcal_invert, 0644, kcal_invert_show, kcal_invert_store);

static struct attribute *ccolorcal_attributes[] = {
    &dev_attr_kcal.attr,
    &dev_attr_kcal_sat.attr,
    &dev_attr_kcal_val.attr,
    &dev_attr_kcal_cont.attr,
    &dev_attr_kcal_hue.attr,
    &dev_attr_kcal_enable.attr,
    &dev_attr_kcal_invert.attr,
    NULL
};

static struct attribute_group ccolorcal_attr_group = {
    .name = "kcal_ctrl",
    .attrs = ccolorcal_attributes,
};

static int ccolorcal_probe(struct platform_device *pdev) {
    mutex_init(&ccolorcal.lock);
    fb_register_client(&ccolorcal_fb_notif);
    return sysfs_create_group(&pdev->dev.kobj, &ccolorcal_attr_group);
}

static int ccolorcal_remove(struct platform_device *pdev) {
    fb_unregister_client(&ccolorcal_fb_notif);
    sysfs_remove_group(&pdev->dev.kobj, &ccolorcal_attr_group);
    return 0;
}

static struct platform_driver ccolorcal_driver = {
    .probe = ccolorcal_probe,
    .remove = ccolorcal_remove,
    .driver = {
        .name = "ccolorcal",
        .owner = THIS_MODULE,
    },
};

static int __init ccolorcal_init(void) {
    int ret = platform_driver_register(&ccolorcal_driver);
    if (ret) return ret;

    // Tạo device ảo để trigger hàm probe
    ccolorcal_pdev = platform_device_register_simple("ccolorcal", -1, NULL, 0);
    
    // if (num_registered_fb > 0) {
    //     mfd = (struct msm_fb_data_type *)registered_fb[0]->par;
    // }
    return 0;
}

static void __exit ccolorcal_exit(void) {
    platform_device_unregister(ccolorcal_pdev);
    platform_driver_unregister(&ccolorcal_driver);
}

MODULE_AUTHOR("Noveris");
MODULE_DESCRIPTION("CColorCal - Cinnamon Color Calibration");
MODULE_LICENSE("GPL v2");

module_init(ccolorcal_init);
module_exit(ccolorcal_exit);
