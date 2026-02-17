/*
 * zcomp_cinnamon.c - Cinnamon Compression Engine (Turbo Optimized)
 *
 * Optimizations:
 * 1. Branchless Zero Check: Reduces CPU pipeline stalls.
 * 2. Pressure Switch: Reads atomic state once per page, skips memcpy under load.
 * 3. Compiler Hints: likely/unlikely for better instruction scheduling.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <asm/unaligned.h>
#include <crypto/cinnamon.h>

#include "zcomp.h"
#include "zcomp_cinnamon.h"

extern atomic_t cinnamon_io_pressure;

/* Constants */
#define CINNAMON_BLOCK_SIZE 32
#define CINNAMON_BLOCKS_PER_PAGE (PAGE_SIZE / CINNAMON_BLOCK_SIZE)
#define CINNAMON_HEADER_SIZE (CINNAMON_BLOCKS_PER_PAGE / 8)

#define CINNAMON_MODE_RAW     0
#define CINNAMON_MODE_ZERO    1
#define CINNAMON_MODE_MATCH   2

/* * Tối ưu hóa: Dùng OR (|) thay vì AND (&&) 
 * Giúp CPU so sánh gộp, tránh ngắt quãng pipeline (Branchless approach)
 */
static inline int is_zero_block(const char *block)
{
	const u64 *p = (const u64 *)block;
	return (get_unaligned_le64(&p[0]) | get_unaligned_le64(&p[1]) |
		get_unaligned_le64(&p[2]) | get_unaligned_le64(&p[3])) == 0;
}

static inline int blocks_match(const char *b1, const char *b2)
{
	const u64 *p1 = (const u64 *)b1;
	const u64 *p2 = (const u64 *)b2;
	/* So sánh từng cặp u64 để tận dụng thanh ghi 64-bit */
	return (get_unaligned_le64(&p1[0]) == get_unaligned_le64(&p2[0]) &&
		get_unaligned_le64(&p1[1]) == get_unaligned_le64(&p2[1]) &&
		get_unaligned_le64(&p1[2]) == get_unaligned_le64(&p2[2]) &&
		get_unaligned_le64(&p1[3]) == get_unaligned_le64(&p2[3]));
}

static int cinnamon_compress(const unsigned char *src, unsigned char *dst,
			    size_t *dst_len, void *private)
{
	int i;
	unsigned char *header_ptr = dst;
	unsigned char *dst_data = dst + CINNAMON_HEADER_SIZE;
	size_t out_len = CINNAMON_HEADER_SIZE;
	u64 final_checksum;
	
	/* Tối ưu: Đọc áp lực 1 lần duy nhất ở đầu hàm */
	int pressure = atomic_read(&cinnamon_io_pressure);
	bool allow_match = (pressure < 2);

	/* Stack-allocated buffer */
	u64 prev_block[CINNAMON_BLOCK_SIZE / sizeof(u64)] __aligned(8);

	memset(prev_block, 0, sizeof(prev_block));
	memset(header_ptr, 0, CINNAMON_HEADER_SIZE);

	for (i = 0; i < CINNAMON_BLOCKS_PER_PAGE; i++) {
		const char *current_src = (const char *)src + (i * CINNAMON_BLOCK_SIZE);
		bool is_compressed = false;

		/* 1. Zero Check: Trường hợp phổ biến nhất -> likely */
		if (likely(is_zero_block(current_src))) {
			header_ptr[i/8] |= (1 << (i%8));
			*dst_data++ = CINNAMON_MODE_ZERO;
			out_len += 1;
			is_compressed = true;
		} 
		/* 2. Match Check: Chỉ chạy khi áp lực thấp */
		else if (allow_match && blocks_match(current_src, (char *)prev_block)) {
			header_ptr[i/8] |= (1 << (i%8));
			*dst_data++ = CINNAMON_MODE_MATCH;
			out_len += 1;
			is_compressed = true;
		}

		/* 3. Raw Copy */
		if (!is_compressed) {
			if (unlikely(out_len + CINNAMON_BLOCK_SIZE > *dst_len - sizeof(u64)))
				goto fallback_raw;

			memcpy(dst_data, current_src, CINNAMON_BLOCK_SIZE);
			dst_data += CINNAMON_BLOCK_SIZE;
			out_len += CINNAMON_BLOCK_SIZE;
		}

		/* * Tối ưu cực mạnh:
		 * Nếu đang áp lực cao (allow_match = false), ta KHÔNG CẦN memcpy prev_block.
		 * Tiết kiệm 128 lần memcpy mỗi Page khi máy đang lag!
		 */
		if (allow_match)
			memcpy(prev_block, current_src, CINNAMON_BLOCK_SIZE);
	}

	final_checksum = cinnamon_checksum(dst, out_len);
	
	/* Kiểm tra bound lần cuối trước khi ghi checksum */
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

static int cinnamon_decompress(const unsigned char *src, size_t src_len,
			      unsigned char *dst)
{
	int i;
	const unsigned char *header_ptr;
	const unsigned char *src_data;
	size_t consumed_bytes;
	u64 stored_checksum;
	u64 actual_checksum;
	u64 prev_block[CINNAMON_BLOCK_SIZE / sizeof(u64)] __aligned(8);

	if (unlikely(src_len == PAGE_SIZE)) {
		memcpy(dst, src, PAGE_SIZE);
		return 0;
	}

	if (unlikely(src_len < sizeof(u64) + CINNAMON_HEADER_SIZE))
		return -EINVAL;

	header_ptr = src;
	src_data = src + CINNAMON_HEADER_SIZE;
	consumed_bytes = CINNAMON_HEADER_SIZE;

	memset(prev_block, 0, sizeof(prev_block));

	stored_checksum = get_unaligned_le64(src + src_len - sizeof(u64));
	actual_checksum = cinnamon_checksum(src, src_len - sizeof(u64));

	if (unlikely(stored_checksum != actual_checksum)) {
		return -EIO;
	}

	for (i = 0; i < CINNAMON_BLOCKS_PER_PAGE; i++) {
		char *current_dst = (char *)dst + (i * CINNAMON_BLOCK_SIZE);
		/* Tách bit header */
		bool is_compressed = (header_ptr[i/8] >> (i%8)) & 1;

		if (is_compressed) {
			unsigned char mode = *src_data++;
			consumed_bytes += 1;

			if (mode == CINNAMON_MODE_ZERO) {
				memset(current_dst, 0, CINNAMON_BLOCK_SIZE);
			} else if (mode == CINNAMON_MODE_MATCH) {
				memcpy(current_dst, prev_block, CINNAMON_BLOCK_SIZE);
			} else {
				return -EINVAL;
			}
		} else {
			if (unlikely(consumed_bytes + CINNAMON_BLOCK_SIZE > src_len))
				return -EINVAL;
			memcpy(current_dst, src_data, CINNAMON_BLOCK_SIZE);
			src_data += CINNAMON_BLOCK_SIZE;
			consumed_bytes += CINNAMON_BLOCK_SIZE;
		}
		
		/* Giải nén luôn cần prev_block chính xác, không được skip */
		memcpy(prev_block, current_dst, CINNAMON_BLOCK_SIZE);
	}

	return 0;
}

static void *cinnamon_create(void)
{
	struct cinnamon_ctx *ctx;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	return ctx;
}

static void cinnamon_destroy(void *private)
{
	kfree(private);
}

struct zcomp_backend zcomp_cinnamon = {
	.compress = cinnamon_compress,
	.decompress = cinnamon_decompress,
	.create = cinnamon_create,
	.destroy = cinnamon_destroy,
	.name = "cinnamon",
};