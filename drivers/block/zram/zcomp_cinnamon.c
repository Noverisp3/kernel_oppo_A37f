/*
 * zcomp_cinnamon.c - Cinnamon Compression Engine with Delta support
 *
 * Optimizations:
 * 1. Branchless Zero Check: OR-based comparison to reduce pipeline stalls.
 * 2. Pressure Switch: Atomic read once per page, skip match under I/O load.
 * 3. Compiler Hints: likely/unlikely for better branch prediction.
 * 4. Prefetching: Load next blocks into cache to minimize latency.
 * 5. 4 Prev Blocks: Ring buffer for periodicity matching (10-15% more matches).
 * 6. Delta Compression: Detect blocks that differ in 1-2 bytes from previous blocks.
 * 7. 3-Bit Header: Encode specific match indices, header size 48 bytes.
 * 8. Local Counters: Batch atomic updates to reduce bus contention.
 * 9. Proc Monitoring: /proc/ccompress exposes stats, ratios, and savings.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <asm/unaligned.h>
#include <crypto/cinnamon.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "zcomp.h"
#include "zcomp_cinnamon.h"

extern atomic_t cinnamon_io_pressure;

/* Compression statistics */
atomic_t cinnamon_pages_compressed = ATOMIC_INIT(0);
atomic_t cinnamon_zero_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_match_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_delta_blocks = ATOMIC_INIT(0);   /* New: blocks compressed with delta */
atomic_t cinnamon_raw_blocks = ATOMIC_INIT(0);

/* Constants */
#define CINNAMON_BLOCK_SIZE 32
#define CINNAMON_BLOCKS_PER_PAGE (PAGE_SIZE / CINNAMON_BLOCK_SIZE)
#define CINNAMON_HEADER_SIZE (CINNAMON_BLOCKS_PER_PAGE / 2)

#define CINNAMON_MODE_RAW     0
#define CINNAMON_MODE_ZERO    1
#define CINNAMON_MODE_MATCH0  2
#define CINNAMON_MODE_MATCH1  3
#define CINNAMON_MODE_MATCH2  4
#define CINNAMON_MODE_MATCH3  5
#define CINNAMON_MODE_DELTA1  6   /* 1-byte difference */
#define CINNAMON_MODE_DELTA2  7   /* 2-byte difference */
#define CINNAMON_MODE_DELTA3  8   /* 3-byte difference */

/* Đếm số byte khác nhau giữa hai block 32 byte, dùng SWAR, dừng sớm nếu >3 */
static inline int count_diff_bytes(const char *b1, const char *b2)
{
    const u64 *p1 = (const u64 *)b1;
    const u64 *p2 = (const u64 *)b2;
    int i, total = 0;
	int cnt;

    for (i = 0; i < 4; i++) {
        u64 xor = get_unaligned_le64(&p1[i]) ^ get_unaligned_le64(&p2[i]);
        if (xor == 0)
            continue;

        /* Đếm số byte khác trong khối 8 byte này */
    	cnt = 0;
        if (xor & 0xFF00000000000000ULL) cnt++;
        if (xor & 0x00FF000000000000ULL) cnt++;
        if (xor & 0x0000FF0000000000ULL) cnt++;
        if (xor & 0x000000FF00000000ULL) cnt++;
        if (xor & 0x00000000FF000000ULL) cnt++;
        if (xor & 0x0000000000FF0000ULL) cnt++;
        if (xor & 0x000000000000FF00ULL) cnt++;
        if (xor & 0x00000000000000FFULL) cnt++;

        total += cnt;
        if (total > 3)
            return total;  /* thoát sớm */
    }
    return total;
}

/* Helper: find offset of first differing byte */
static inline int find_first_diff_byte(const char *b1, const char *b2)
{
	int i;
	const u8 *p1 = (const u8 *)b1;
	const u8 *p2 = (const u8 *)b2;
	for (i = 0; i < CINNAMON_BLOCK_SIZE; i++) {
		if (p1[i] != p2[i])
			return i;
	}
	return 0; /* should not happen */
}

/* Helper: find offsets and values of two differing bytes (assumes exactly 2) */
static inline void find_two_diff_bytes(const char *b1, const char *b2,
				       int *off1, u8 *val1, int *off2, u8 *val2)
{
	int i, found = 0;
	const u8 *p1 = (const u8 *)b1;
	const u8 *p2 = (const u8 *)b2;
	for (i = 0; i < CINNAMON_BLOCK_SIZE && found < 2; i++) {
		if (p1[i] != p2[i]) {
			if (found == 0) {
				*off1 = i;
				*val1 = p1[i];
			} else {
				*off2 = i;
				*val2 = p1[i];
			}
			found++;
		}
	}
}

/* Helper: find offsets and values of three differing bytes */
static inline void find_three_diff_bytes(const char *b1, const char *b2,
                                         int *off1, u8 *val1,
                                         int *off2, u8 *val2,
                                         int *off3, u8 *val3)
{
    int i, found = 0;
    const u8 *p1 = (const u8 *)b1;
    const u8 *p2 = (const u8 *)b2;
    for (i = 0; i < CINNAMON_BLOCK_SIZE && found < 3; i++) {
        if (p1[i] != p2[i]) {
            if (found == 0) {
                *off1 = i;
                *val1 = p1[i];
            } else if (found == 1) {
                *off2 = i;
                *val2 = p1[i];
            } else {
                *off3 = i;
                *val3 = p1[i];
            }
            found++;
        }
    }
}

/* Branchless zero check */
static inline int is_zero_block(const char *block)
{
	const u64 *p = (const u64 *)block;
	return (get_unaligned_le64(&p[0]) | get_unaligned_le64(&p[1]) |
		get_unaligned_le64(&p[2]) | get_unaligned_le64(&p[3])) == 0;
}

/* Exact match check */
static inline int blocks_match(const char *b1, const char *b2)
{
	const u64 *p1 = (const u64 *)b1;
	const u64 *p2 = (const u64 *)b2;
	return (get_unaligned_le64(&p1[0]) == get_unaligned_le64(&p2[0]) &&
		get_unaligned_le64(&p1[1]) == get_unaligned_le64(&p2[1]) &&
		get_unaligned_le64(&p1[2]) == get_unaligned_le64(&p2[2]) &&
		get_unaligned_le64(&p1[3]) == get_unaligned_le64(&p2[3]));
}

/* 4-bit nibble header helpers */
static inline void set_header_mode(unsigned char *header, int block_idx, unsigned char mode)
{
    int byte_idx = block_idx >> 1;               /* chia 2 */
    int shift = (block_idx & 1) ? 0 : 4;         /* lẻ: low nibble, chẵn: high nibble */
    unsigned char mask = 0xF << shift;
    unsigned char value = (mode & 0xF) << shift;

    header[byte_idx] = (header[byte_idx] & ~mask) | value;
}

static inline unsigned char get_header_mode(const unsigned char *header, int block_idx)
{
    int byte_idx = block_idx >> 1;
    int shift = (block_idx & 1) ? 0 : 4;
    return (header[byte_idx] >> shift) & 0xF;
}

/* Main compression function */
static int cinnamon_compress(const unsigned char *src, unsigned char *dst,
			    size_t *dst_len, void *private)
{
	int i;
	unsigned char *header_ptr = dst;
	unsigned char *dst_data = dst + CINNAMON_HEADER_SIZE;
	size_t out_len = CINNAMON_HEADER_SIZE;
	u64 final_checksum;
	
	/* Local counters to reduce atomic contention */
	int local_zero = 0, local_match = 0, local_delta = 0, local_raw = 0;

	int pressure = atomic_read(&cinnamon_io_pressure);
	bool allow_match = (pressure < 2);   /* also controls delta attempts */

	/* C90: declare all variables at beginning */
	int best_delta, best_ref, diff_count, offset, off1, off2, off3;
	u8 value, val1, val2, val3;

	struct cinnamon_ctx *ctx = private;
	u64 (*prev_blocks)[CINNAMON_BLOCK_SIZE / sizeof(u64)];
	
	if (unlikely(!ctx))
		return -EINVAL;
	
	prev_blocks = ctx->prev_blocks;

	memset(ctx->prev_blocks, 0, sizeof(ctx->prev_blocks));
	ctx->prev_index = 0;

	memset(header_ptr, 0, CINNAMON_HEADER_SIZE);

	for (i = 0; i < CINNAMON_BLOCKS_PER_PAGE; i++) {
		const char *current_src = (const char *)src + (i * CINNAMON_BLOCK_SIZE);
		unsigned char mode;
		int p;

		/* Prefetch next block */
		if (i + 2 < CINNAMON_BLOCKS_PER_PAGE) {
			__builtin_prefetch((const char *)src + ((i + 2) * CINNAMON_BLOCK_SIZE), 0, 0);
		}

		if (likely(is_zero_block(current_src))) {
			mode = CINNAMON_MODE_ZERO;
			local_zero++;
			goto set_mode;
		}

		/* Try exact match with previous blocks */
		if (allow_match) {
			for (p = 0; p < 4; p++) {
				if (blocks_match(current_src, (char *)prev_blocks[p])) {
					mode = CINNAMON_MODE_MATCH0 + p;
					local_match++;
					goto set_mode;
				}
			}
		}

		/* No exact match -> try delta compression */
		if (allow_match) {
			best_delta = 4;          /* we accept at most 3 differing bytes */
			best_ref = -1;

			for (p = 0; p < 4; p++) {
				diff_count = count_diff_bytes(current_src, (char *)prev_blocks[p]);
				if (diff_count < best_delta) {
					best_delta = diff_count;
					best_ref = p;
					if (best_delta == 0) break; /* exact match already handled */
				}
			}

			if (best_ref >= 0 && best_delta <= 3) {
				if (best_delta == 1) {
					mode = CINNAMON_MODE_DELTA1;
					offset = find_first_diff_byte(current_src, (char *)prev_blocks[best_ref]);
					value = ((u8 *)current_src)[offset];
					if (unlikely(out_len + 3 > *dst_len - sizeof(u64)))
						goto fallback_raw;
					dst_data[0] = best_ref;
					dst_data[1] = offset;
					dst_data[2] = value;
					dst_data += 3;
					out_len += 3;
					local_delta++;
					goto set_mode;
				} else if (best_delta == 2) {
					mode = CINNAMON_MODE_DELTA2;
					find_two_diff_bytes(current_src, (char *)prev_blocks[best_ref],
							    &off1, &val1, &off2, &val2);
					if (unlikely(out_len + 5 > *dst_len - sizeof(u64)))
						goto fallback_raw;
					dst_data[0] = best_ref;
					dst_data[1] = off1;
					dst_data[2] = val1;
					dst_data[3] = off2;
					dst_data[4] = val2;
					dst_data += 5;
					out_len += 5;
					local_delta++;
					goto set_mode;
				} else if (best_delta == 3) {
					mode = CINNAMON_MODE_DELTA3;
					find_three_diff_bytes(current_src, (char *)prev_blocks[best_ref],
								&off1, &val1, &off2, &val2, &off3, &val3);
					if (unlikely(out_len + 7 > *dst_len - sizeof(u64)))
						goto fallback_raw;
					dst_data[0] = best_ref;
					dst_data[1] = off1;
					dst_data[2] = val1;
					dst_data[3] = off2;
					dst_data[4] = val2;
					dst_data[5] = off3;
					dst_data[6] = val3;
					dst_data += 7;
					out_len += 7;
					local_delta++;
					goto set_mode;
				}
			}
		}
		/* Fallback: raw block */
		mode = CINNAMON_MODE_RAW;
		local_raw++;
		if (unlikely(out_len + CINNAMON_BLOCK_SIZE > *dst_len - sizeof(u64)))
			goto fallback_raw;

		memcpy(dst_data, current_src, CINNAMON_BLOCK_SIZE);
		dst_data += CINNAMON_BLOCK_SIZE;
		out_len += CINNAMON_BLOCK_SIZE;

set_mode:
		set_header_mode(header_ptr, i, mode);

		/* Update previous blocks ring buffer if matches/deltas are allowed */
		if (allow_match) {
			memcpy(prev_blocks[ctx->prev_index], current_src, CINNAMON_BLOCK_SIZE);
			ctx->prev_index = (ctx->prev_index + 1) % 4;
		}
	}

	/* Batch update statistics */
	atomic_inc(&cinnamon_pages_compressed);
	atomic_add(local_zero, &cinnamon_zero_blocks);
	atomic_add(local_match, &cinnamon_match_blocks);
	atomic_add(local_delta, &cinnamon_delta_blocks);
	atomic_add(local_raw, &cinnamon_raw_blocks);

	final_checksum = cinnamon_checksum(dst, out_len);
	if (likely(out_len + sizeof(u64) <= *dst_len)) {
		put_unaligned_le64(final_checksum, dst_data);
		*dst_len = out_len + sizeof(u64);
	} else {
		goto fallback_raw;
	}
	return 0;

fallback_raw:
	memcpy(dst, src, PAGE_SIZE);
	*dst_len = PAGE_SIZE;
	return 0;
}

/* Decompression */
static int cinnamon_decompress(const unsigned char *src, size_t src_len,
			      unsigned char *dst)
{
	int i;
	const unsigned char *header_ptr;
	const unsigned char *src_data;
	size_t consumed_bytes;
	u64 stored_checksum;
	u64 actual_checksum;
	u64 prev_blocks[4][CINNAMON_BLOCK_SIZE / sizeof(u64)] __aligned(8);
	int prev_index;
	int match_idx, ref, offset, off1, off2, off3;
	u8 value, val1, val2, val3;

	if (unlikely(src_len == PAGE_SIZE)) {
		memcpy(dst, src, PAGE_SIZE);
		return 0;
	}

	if (unlikely(src_len < sizeof(u64) + CINNAMON_HEADER_SIZE))
		return -EINVAL;

	header_ptr = src;
	src_data = src + CINNAMON_HEADER_SIZE;
	consumed_bytes = CINNAMON_HEADER_SIZE;

	memset(prev_blocks, 0, sizeof(prev_blocks));
	prev_index = 0;

	stored_checksum = get_unaligned_le64(src + src_len - sizeof(u64));
	actual_checksum = cinnamon_checksum(src, src_len - sizeof(u64));

	if (unlikely(stored_checksum != actual_checksum))
		return -EIO;

	for (i = 0; i < CINNAMON_BLOCKS_PER_PAGE; i++) {
		char *current_dst = (char *)dst + (i * CINNAMON_BLOCK_SIZE);
		unsigned char mode = get_header_mode(header_ptr, i);

		if (mode == CINNAMON_MODE_RAW) {
			if (unlikely(consumed_bytes + CINNAMON_BLOCK_SIZE > src_len - sizeof(u64)))
				return -EINVAL;
			memcpy(current_dst, src_data, CINNAMON_BLOCK_SIZE);
			src_data += CINNAMON_BLOCK_SIZE;
			consumed_bytes += CINNAMON_BLOCK_SIZE;
		} else if (mode == CINNAMON_MODE_ZERO) {
			memset(current_dst, 0, CINNAMON_BLOCK_SIZE);
		} else if (mode >= CINNAMON_MODE_MATCH0 && mode <= CINNAMON_MODE_MATCH3) {
			match_idx = mode - CINNAMON_MODE_MATCH0;
			memcpy(current_dst, prev_blocks[match_idx], CINNAMON_BLOCK_SIZE);
		} else if (mode == CINNAMON_MODE_DELTA1) {
			if (unlikely(consumed_bytes + 3 > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			offset = src_data[1];
			value = src_data[2];
			if (ref < 0 || ref >= 4)
				return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], CINNAMON_BLOCK_SIZE);
			current_dst[offset] = value;
			src_data += 3;
			consumed_bytes += 3;
		} else if (mode == CINNAMON_MODE_DELTA2) {
			if (unlikely(consumed_bytes + 5 > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			off1 = src_data[1];
			val1 = src_data[2];
			off2 = src_data[3];
			val2 = src_data[4];
			if (ref < 0 || ref >= 4)
				return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], CINNAMON_BLOCK_SIZE);
			current_dst[off1] = val1;
			current_dst[off2] = val2;
			src_data += 5;
			consumed_bytes += 5;
        } else if (mode == CINNAMON_MODE_DELTA3) {
            if (unlikely(consumed_bytes + 7 > src_len - sizeof(u64)))
                return -EINVAL;
            ref = src_data[0];
            off1 = src_data[1];
            val1 = src_data[2];
            off2 = src_data[3];
            val2 = src_data[4];
            off3 = src_data[5];
            val3 = src_data[6];
            if (ref < 0 || ref >= 4)
                return -EINVAL;
            memcpy(current_dst, prev_blocks[ref], CINNAMON_BLOCK_SIZE);
            current_dst[off1] = val1;
            current_dst[off2] = val2;
            current_dst[off3] = val3;
            src_data += 7;
            consumed_bytes += 7;
        } else {
            return -EINVAL;
        }

		/* Update previous blocks ring buffer */
		memcpy(prev_blocks[prev_index], current_dst, CINNAMON_BLOCK_SIZE);
		prev_index = (prev_index + 1) % 4;
	}

	if (consumed_bytes != src_len - sizeof(u64))
		return -EINVAL;

	return 0;
}

static void *cinnamon_create(void)
{
	struct cinnamon_ctx *ctx;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (unlikely(!ctx))
		return NULL;
	return ctx;
}

static void cinnamon_destroy(void *private)
{
	kfree(private);
}

/* Proc file system interface */
static int cinnamon_proc_show(struct seq_file *m, void *v)
{
	unsigned long total_p, raw_b;

	seq_printf(m, "io_pressure: %d\n", atomic_read(&cinnamon_io_pressure));
	seq_printf(m, "pages_compressed: %d\n", atomic_read(&cinnamon_pages_compressed));
	seq_printf(m, "zero_blocks: %d\n", atomic_read(&cinnamon_zero_blocks));
	seq_printf(m, "match_blocks: %d\n", atomic_read(&cinnamon_match_blocks));
	seq_printf(m, "delta_blocks: %d\n", atomic_read(&cinnamon_delta_blocks));
	seq_printf(m, "raw_blocks: %d\n", atomic_read(&cinnamon_raw_blocks));

	total_p = (unsigned long)atomic_read(&cinnamon_pages_compressed);
	raw_b = (unsigned long)atomic_read(&cinnamon_raw_blocks);

	if (total_p > 0) {
		unsigned long input_sz = total_p * PAGE_SIZE;
		unsigned long output_sz = (total_p * CINNAMON_HEADER_SIZE) +
					   (raw_b * CINNAMON_BLOCK_SIZE) +
					   (total_p * sizeof(u64));
		unsigned int ratio = (output_sz * 100) / input_sz;

		seq_printf(m, "compression_ratio: %u%%\n", ratio);
		seq_printf(m, "ram_saved: %lu KB\n", (input_sz - output_sz) / 1024);
	}

	return 0;
}

static int cinnamon_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cinnamon_proc_show, NULL);
}

static const struct file_operations cinnamon_proc_fops = {
	.open = cinnamon_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void cinnamon_proc_init(void)
{
	proc_create("ccompress", 0444, NULL, &cinnamon_proc_fops);
}

void cinnamon_proc_exit(void)
{
	remove_proc_entry("ccompress", NULL);
}

/* Export the backend structure for zram */
struct zcomp_backend zcomp_cinnamon = {
	.compress = cinnamon_compress,
	.decompress = cinnamon_decompress,
	.create = cinnamon_create,
	.destroy = cinnamon_destroy,
	.name = "cinnamon",
};