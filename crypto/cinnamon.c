/*
 * crypto/cinnamon.c - Cinnamon Crypto Module (Fixed & Optimized)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/unaligned.h> /* Rất quan trọng để chống Alignment Fault */
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "cinnamon.h"

#define CINNAMON_SEED 0x9E3779B97F4A7C15ULL /* Golden ratio-based seed for optimal diffusion */ 
#define CINNAMON_ROTATE_BITS 13 
#define CINNAMON_ZERO_CONSTANT 0x5A5A5A5A5A5A5A5AULL

static u64 last_checksum;

static ssize_t cinnamon_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    char tmp[32];
    int len = sprintf(tmp, "0x%016llx\n", last_checksum);
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t cinnamon_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char *kbuf;
    size_t len = count;
    if (len > PAGE_SIZE - 1)
        len = PAGE_SIZE - 1;
    kbuf = kmalloc(len + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, buf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[len] = '\0';
    // Remove trailing newline
    if (len > 0 && kbuf[len-1] == '\n')
        kbuf[--len] = '\0';
    last_checksum = cinnamon_checksum((u8 *)kbuf, len);
    kfree(kbuf);
    return count;
}

static const struct file_operations cinnamon_proc_fops = {
    .owner = THIS_MODULE,
    .read = cinnamon_proc_read,
    .write = cinnamon_proc_write,
};

/*
 * Check if a 32-byte block is all zeros (Fixed logic)
 */
static inline bool is_zero_block_32(const u8 *block)
{
    /* Kiểm tra toàn bộ 32 byte thông qua u64 để đạt tốc độ ánh sáng */
    const u64 *p = (const u64 *)block;
    /* Dùng get_unaligned ở đây nếu block không chắc chắn align 8 */
    return (get_unaligned_le64(&p[0]) == 0 && get_unaligned_le64(&p[1]) == 0 &&
            get_unaligned_le64(&p[2]) == 0 && get_unaligned_le64(&p[3]) == 0);
}

/*
 * Cinnamon XOR-Rotate checksum calculation (Fixed Alignment)
 */
u64 cinnamon_checksum(const u8 *data, size_t len)
{
    u64 checksum = CINNAMON_SEED;
    size_t i;

    for (i = 0; i + 8 <= len; i += 8) {
        /* Đọc 8 byte an toàn dù địa chỉ là bất kỳ đâu */
        u64 chunk = get_unaligned_le64(data + i);
        
        if (chunk == 0) {
            checksum ^= CINNAMON_ZERO_CONSTANT;
        } else {
            checksum ^= chunk;
            checksum = (checksum << CINNAMON_ROTATE_BITS) |
                       (checksum >> (64 - CINNAMON_ROTATE_BITS));
        }
    }
    
    /* Xử lý các byte lẻ cuối cùng (nếu len không chia hết cho 8) */
    if (len % 8) {
        u64 tail = 0;
        memcpy(&tail, data + i, len % 8);
        checksum ^= tail;
    }

    return checksum;
}

/*
 * Verify Cinnamon checksum
 */
bool cinnamon_verify(const u8 *data, size_t len, u64 expected_checksum)
{
    return cinnamon_checksum(data, len) == expected_checksum;
}
EXPORT_SYMBOL(cinnamon_checksum);
EXPORT_SYMBOL(cinnamon_verify);

/* * Inline checksum cho CCompress gọi 
 */
u64 cinnamon_compress_checksum(const u8 *data, size_t len,
                             cinnamon_compress_cb compress_cb, void *cb_data)
{
    u64 checksum = CINNAMON_SEED;
    size_t i;

    for (i = 0; i + 8 <= len; i += 8) {
        u64 chunk = get_unaligned_le64(data + i);

        if (compress_cb) {
            /* Callback để nén song song với tính checksum */
            if (compress_cb(data + i, 8, chunk, cb_data))
                return 0;
        }

        if (chunk == 0) {
            checksum ^= CINNAMON_ZERO_CONSTANT;
        } else {
            checksum ^= chunk;
            checksum = (checksum << CINNAMON_ROTATE_BITS) |
                       (checksum >> (64 - CINNAMON_ROTATE_BITS));
        }
    }
    return checksum;
}
EXPORT_SYMBOL(cinnamon_compress_checksum);

static int __init cinnamon_crypto_init(void)
{
    pr_info("Cinnamon Crypto: XOR-Rotate engine initialized\n");
    if (!proc_create("cinnamon_checksum", 0644, NULL, &cinnamon_proc_fops)) {
        pr_err("Failed to create /proc/cinnamon_checksum\n");
        return -ENOMEM;
    }
    return 0;
}

static void __exit cinnamon_crypto_exit(void)
{
    remove_proc_entry("cinnamon_checksum", NULL);
    pr_info("Cinnamon Crypto: unloaded\n");
}

module_init(cinnamon_crypto_init);
module_exit(cinnamon_crypto_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cinnamon Crypto - Ultra Fast Checksum for MSM8916");