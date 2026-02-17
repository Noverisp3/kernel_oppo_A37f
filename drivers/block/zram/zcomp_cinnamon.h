/*
 * zcomp_cinnamon.h - Cinnamon Compression for zRAM
 */

#ifndef _ZCOMP_CINNAMON_H_
#define _ZCOMP_CINNAMON_H_

#include <linux/types.h>
#include <linux/atomic.h>

/* Cấu trúc Context cho mỗi luồng nén */
struct cinnamon_ctx {
    /* No shared state needed - each compression uses local variables */
    /* This prevents race conditions in multi-core environments */
};

/* * Khai báo biến áp lực I/O từ I/O Scheduler.
 * Dùng extern atomic_t để đảm bảo tính an toàn khi truy cập đa nhân (Symmetric Multiprocessing)
 */
extern atomic_t cinnamon_io_pressure;

/* Khai báo backend để zRAM có thể nhận diện */
extern struct zcomp_backend zcomp_cinnamon;

#endif /* _ZCOMP_CINNAMON_H_ */