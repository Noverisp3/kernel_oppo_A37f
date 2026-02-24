/*
 * zcomp_cinnamon.h - Cinnamon Compression for zRAM
 */

#ifndef _ZCOMP_CINNAMON_H_
#define _ZCOMP_CINNAMON_H_

#include <linux/types.h>
#include <linux/atomic.h>

#define CINNAMON_BLOCK_SIZE 32

/* Cấu trúc Context cho mỗi luồng nén */
struct cinnamon_ctx {
    u64 prev_blocks[4][CINNAMON_BLOCK_SIZE / sizeof(u64)];
    int prev_index;
};

/* * Khai báo biến áp lực I/O từ I/O Scheduler.
 * Dùng extern atomic_t để đảm bảo tính an toàn khi truy cập đa nhân (Symmetric Multiprocessing)
 */
extern atomic_t cinnamon_io_pressure;

/* Compression statistics */
extern atomic_t cinnamon_pages_compressed;
extern atomic_t cinnamon_zero_blocks;
extern atomic_t cinnamon_match_blocks;
extern atomic_t cinnamon_delta_blocks;
extern atomic_t cinnamon_raw_blocks;

/* Proc entry functions */
extern void cinnamon_proc_init(void);
extern void cinnamon_proc_exit(void);

/* Khai báo backend để zRAM có thể nhận diện */
extern struct zcomp_backend zcomp_cinnamon;

#endif /* _ZCOMP_CINNAMON_H_ */