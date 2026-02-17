/*
 * crypto/cinnamon.h - Cinnamon Crypto Header (Final Version)
 */

#ifndef _CRYPTO_CINNAMON_H
#define _CRYPTO_CINNAMON_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/crypto.h>

/* * Callback cho nén inline: 
 * Trả về 0 nếu thành công, khác 0 để dừng quá trình nén 
 */
typedef int (*cinnamon_compress_cb)(const u8 *data, size_t len, u64 chunk, void *cb_data);

/* --- API Core --- */

/**
 * cinnamon_checksum - Tính toán checksum siêu tốc XOR-Rotate 64-bit
 */
u64 cinnamon_checksum(const u8 *data, size_t len);

/**
 * cinnamon_verify - Xác thực dữ liệu với checksum cho trước
 */
bool cinnamon_verify(const u8 *data, size_t len, u64 expected_checksum);

/**
 * cinnamon_compress_checksum - Tính checksum song song khi đang nén
 * Giúp giảm số lần duyệt RAM (Memory pass), tối ưu bộ nhớ L1/L2
 */
u64 cinnamon_compress_checksum(const u8 *data, size_t len,
			      cinnamon_compress_cb compress_cb, void *cb_data);

/* * Export các hàm này để các module khác (như zram) có thể thấy 
 * (Cần EXPORT_SYMBOL trong file .c tương ứng)
 */

#endif /* _CRYPTO_CINNAMON_H */