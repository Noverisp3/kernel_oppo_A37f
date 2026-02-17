/*
 * cinnamon-iosched.c - Cinnamon I/O Scheduler v2 (Zero-Alloc & Anti-Starvation)
 *
 * Optimizations:
 * 1. Zero-Alloc: Embeds timestamp directly in request pointer (no kmalloc).
 * 2. Anti-Starvation: Forces writes if delayed > 250ms to prevent FS locking.
 * 3. Smart Batching: Dynamic pressure reporting to zcomp backend.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/jiffies.h>

/* TUNABLES */
#define CINNAMON_WRITE_EXPIRE  (HZ / 8)  /* 125ms - Faster response */
#define CINNAMON_FIFO_BATCH    16        /* Larger batch for better throughput */
#define CINNAMON_PRESSURE_THRES 24       /* Lower threshold for earlier pressure reporting */

struct cinnamon_data {
	struct list_head read_queue;
	struct list_head write_queue;
	unsigned int batch_count;
	/* Stats for pressure monitoring */
	unsigned int write_cnt;
	unsigned int read_cnt;
};

/* * Global atomic variable for CC engine
 * 0: Idle
 * 1: Read heavy (Urgent)
 * 2: Write heavy (Batching)
 * 3: Congested (Stop compressing, flush disk!)
 */
atomic_t cinnamon_io_pressure = ATOMIC_INIT(0);
EXPORT_SYMBOL(cinnamon_io_pressure);

static void cinnamon_merged_requests(struct request_queue *q, struct request *rq,
				      struct request *next)
{
	/* 1. Đưa khai báo biến lên đầu hàm (Đúng chuẩn C90) */
	struct cinnamon_data *cd = q->elevator->elevator_data;

	/* 2. Sau đó mới đến các câu lệnh thực thi */
	list_del_init(&next->queuelist);
	
	if (rq_data_dir(next) == WRITE)
		cd->write_cnt--;
	else
		cd->read_cnt--;
}

static int cinnamon_dispatch(struct request_queue *q, int force)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	struct request *rq = NULL;
	unsigned long now = jiffies;
	int current_pressure = 0;

	/* 1. Kiểm tra Starvation (Chống treo máy khi ghi nặng) */
	if (!list_empty(&cd->write_queue)) {
		struct request *wrq = list_entry(cd->write_queue.next, struct request, queuelist);
		unsigned long fifo_time = (unsigned long)wrq->elv.priv[0];

		/* Nếu lệnh ghi đã đợi quá lâu, ưu tiên nó ngay lập tức! */
		if (time_after(now, fifo_time + CINNAMON_WRITE_EXPIRE)) {
			rq = wrq;
			current_pressure = 3; /* CONGESTED */
			goto dispatch_write;
		}
	}

	/* 2. Ưu tiên Đọc (Normal operation) */
	if (!list_empty(&cd->read_queue)) {
		rq = list_entry(cd->read_queue.next, struct request, queuelist);
		current_pressure = 1; /* READ URGENT */
		goto dispatch_read;
	}

	/* 3. Xử lý Ghi theo lô (Batching) */
	if (!list_empty(&cd->write_queue)) {
		rq = list_entry(cd->write_queue.next, struct request, queuelist);
		
		/* Logic Batching đơn giản */
		if (cd->batch_count < CINNAMON_FIFO_BATCH) {
			cd->batch_count++;
			current_pressure = 2; /* WRITE BATCH */
		} else {
			/* Hết batch, nhường một nhịp (nhưng ở đây ko có read nên cứ chạy) */
			cd->batch_count = 0;
			current_pressure = 2;
		}
		goto dispatch_write;
	}

	/* Không còn lệnh nào */
	atomic_set(&cinnamon_io_pressure, 0);
	return 0;

dispatch_read:
	if (rq) {
		cd->read_cnt--;
		list_del_init(&rq->queuelist);
		atomic_set(&cinnamon_io_pressure, current_pressure);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;

dispatch_write:
	if (rq) {
		cd->write_cnt--;
		/* Xóa timestamp (không cần free vì không alloc) */
		rq->elv.priv[0] = NULL; 
		list_del_init(&rq->queuelist);
		
		/* Cập nhật áp lực dựa trên độ dài hàng đợi */
		if (cd->write_cnt > CINNAMON_PRESSURE_THRES)
			current_pressure = 3; /* Force ZRAM to use RAW mode */
			
		atomic_set(&cinnamon_io_pressure, current_pressure);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static void cinnamon_add_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;

	if (rq_data_dir(rq) == READ) {
		list_add_tail(&rq->queuelist, &cd->read_queue);
		cd->read_cnt++;
	} else {
		/* ZERO-ALLOC TRICK: Lưu jiffies trực tiếp vào con trỏ */
		rq->elv.priv[0] = (void *)jiffies;
		list_add_tail(&rq->queuelist, &cd->write_queue);
		cd->write_cnt++;
	}
}

static struct request *
cinnamon_former_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &cd->read_queue ||
	    rq->queuelist.prev == &cd->write_queue)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
cinnamon_latter_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;

	if (rq->queuelist.next == &cd->read_queue ||
	    rq->queuelist.next == &cd->write_queue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

/* Tương thích với kernel 3.10+ (trả về int hoặc void*) */
static int cinnamon_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct cinnamon_data *cd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	cd = kmalloc_node(sizeof(*cd), GFP_KERNEL, q->node);
	if (!cd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&cd->read_queue);
	INIT_LIST_HEAD(&cd->write_queue);
	cd->batch_count = 0;
	cd->write_cnt = 0;
	cd->read_cnt = 0;
	
	eq->elevator_data = cd;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void cinnamon_exit_queue(struct elevator_queue *e)
{
	struct cinnamon_data *cd = e->elevator_data;
	
	/* Không cần giải phóng request wrapper vì ta không alloc! */
	BUG_ON(!list_empty(&cd->read_queue));
	BUG_ON(!list_empty(&cd->write_queue));

	kfree(cd);
}

static ssize_t cinnamon_pressure_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", atomic_read(&cinnamon_io_pressure));
}

static struct elv_fs_entry cinnamon_attrs[] = {
	__ATTR(pressure, 0444, cinnamon_pressure_show, NULL),
	__ATTR_NULL
};

static struct elevator_type elevator_cinnamon = {
	.ops = {
		.elevator_merge_req_fn		= cinnamon_merged_requests,
		.elevator_dispatch_fn		= cinnamon_dispatch,
		.elevator_add_req_fn		= cinnamon_add_request,
		.elevator_former_req_fn		= cinnamon_former_request,
		.elevator_latter_req_fn		= cinnamon_latter_request,
		.elevator_init_fn		= cinnamon_init_queue,
		.elevator_exit_fn		= cinnamon_exit_queue,
	},
	.elevator_name = "cinnamon",
	.elevator_owner = THIS_MODULE,
	.elevator_attrs = cinnamon_attrs,
};

static int __init cinnamon_init(void)
{
	/* Không cần tạo slab cache nữa - Zero Alloc! */
	return elv_register(&elevator_cinnamon);
}

static void __exit cinnamon_exit(void)
{
	elv_unregister(&elevator_cinnamon);
}

module_init(cinnamon_init);
module_exit(cinnamon_exit);

MODULE_AUTHOR("Noveris");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cinnamon I/O Scheduler v2 (Turbo)");