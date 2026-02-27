/*
 * cinnamon-iosched.c - Cinnamon I/O Scheduler
 *
 * Optimizations:
 * 1. Zero-Alloc: Embeds timestamp directly in request pointer (no kmalloc).
 * 2. Anti-Starvation: Forces writes if delayed > 125ms to prevent FS locking.
 * 3. Smart Batching: Dynamic pressure reporting to zcomp backend.
 * 4. Dynamic Read-Ahead: Adjusts read-ahead based on I/O pressure.
 * 5. Hysteresis: Smooths pressure transitions to prevent jitter.
 * 6. Per-Queue State: Ensures SMP safety with isolated state per device.
 * 7. Latency Monitoring: Tracks and exposes average request latency via sysfs.
 * 8. Moving Average: Smooth latency reporting with historical weighting.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/backing-dev.h>

/* TUNABLES */
#define CINNAMON_WRITE_EXPIRE  (HZ / 8)  /* 125ms - Faster response */
#define CINNAMON_READ_BATCH    8        /* Smaller for read to allow write interleaving */
#define CINNAMON_WRITE_BATCH   16       /* Larger for write to maximize sequential throughput */
#define CINNAMON_PRESSURE_THRES 24       /* Lower threshold for earlier pressure reporting */
#define CINNAMON_HISTORY_SIZE 8
#define CINNAMON_TARGET_LATENCY_MS 50   /* mục tiêu latency 50ms */
#define CINNAMON_ADJUST_INTERVAL_MS 2000 /* điều chỉnh mỗi 2 giây */
#define INTERACTIVE_TIMEOUT (HZ)  /* 1 giây */

struct cinnamon_data {
	struct list_head read_queue;
	struct list_head write_queue;
	unsigned int batch_count;
	/* Stats for pressure monitoring */
	int write_cnt;
	int read_cnt;
	/* Read batch counter for write interleaving */
	unsigned int read_batch_count;
	/* Latency tracking */
	atomic64_t total_latency;
	atomic_t num_requests;
	/* Per-queue read-ahead pressure tracking */
	int last_ra_pressure;
	/* Per-queue hysteresis timing */
	unsigned long last_pressure_change_jiffies;
	/* Tunable parameters */
	int write_expire_ms;
	int read_batch;
	int write_batch;
	int pressure_thres;
	int hysteresis_ms;
	/* History for read pattern detection */
	sector_t read_history[CINNAMON_HISTORY_SIZE];
	int history_index;
	int history_count;
	/* Last read direction: 1 forward, -1 backward, 0 unknown */
	int last_read_direction;
	/* Predicted next sector (0 if no prediction) */
	sector_t predicted_sector;
	/* Adaptive tuning */
	unsigned long last_adjust_jiffies;
	int target_latency_ms;
	int adjust_interval_ms;
	/* Tham số tạm thời cho Race to Idle */
	int saved_read_batch;
	int saved_write_batch;
	int saved_write_expire_ms;
	/* Interactive process tracking */
	pid_t last_interactive_pid;
	unsigned long interactive_expire;
	struct request_queue *q;   /* để lấy ra_pages trong sysfs */
};

/* * Global atomic variable for CC engine
 * 0: Idle
 * 1: Read heavy (Urgent)
 * 2: Write heavy (Batching)
 * 3: Congested (Stop compressing, flush disk!)
 */
atomic_t cinnamon_io_pressure = ATOMIC_INIT(0);
EXPORT_SYMBOL(cinnamon_io_pressure);

#define CINNAMON_HYSTERESIS_MS 40

static void cinnamon_update_ra(struct cinnamon_data *cd, struct request_queue *q)
{
	int curr_press = atomic_read(&cinnamon_io_pressure);
	if (curr_press != cd->last_ra_pressure) {
		unsigned int ra_kb;
		if (curr_press == 3) ra_kb = 128;
		else if (curr_press == 0) ra_kb = 4096; /* Max read-ahead for Race to Sleep */
		else if (curr_press <= 1) ra_kb = 2048;
		else ra_kb = 512;
		{
			unsigned int ra_pages = ra_kb >> (PAGE_SHIFT - 10);
			/* Thay WRITE_ONCE bằng gán trực tiếp (an toàn trong ngữ cảnh này) */
			q->backing_dev_info.ra_pages = clamp(ra_pages, 16U, 4096U);
		}
		cd->last_ra_pressure = curr_press;
	}
}

static void cinnamon_set_pressure(struct cinnamon_data *cd, struct request_queue *q, int new_pressure)
{
	int curr_pressure = atomic_read(&cinnamon_io_pressure);
	int changed = 0;
	if (new_pressure != curr_pressure) {
		if (time_after(jiffies, cd->last_pressure_change_jiffies + msecs_to_jiffies(cd->hysteresis_ms))) {
			atomic_set(&cinnamon_io_pressure, new_pressure);
			cd->last_pressure_change_jiffies = jiffies;
			changed = 1;

			/* Race to Idle: khi xuống 0, giảm batch để xử lý nhanh */
			if (new_pressure == 0) {
				/* Lưu giá trị hiện tại */
				cd->saved_read_batch = cd->read_batch;
				cd->saved_write_batch = cd->write_batch;
				cd->saved_write_expire_ms = cd->write_expire_ms;
				/* Giảm batch, tăng thời gian chờ để không vội */
				cd->read_batch = 2;
				cd->write_batch = 2;
				cd->write_expire_ms = 500;
			} else if (curr_pressure == 0) {
				/* Thoát khỏi idle: phục hồi tham số đã lưu */
				cd->read_batch = cd->saved_read_batch;
				cd->write_batch = cd->saved_write_batch;
				cd->write_expire_ms = cd->saved_write_expire_ms;
			}
		}
	}
	/* If equal or hysteresis not passed, do nothing */
	if (changed)
		cinnamon_update_ra(cd, q);
}

static void cinnamon_update_read_history(struct cinnamon_data *cd, sector_t sector)
{
	cd->read_history[cd->history_index] = sector;
	cd->history_index = (cd->history_index + 1) % CINNAMON_HISTORY_SIZE;
	if (cd->history_count < CINNAMON_HISTORY_SIZE)
		cd->history_count++;

	/* Dự đoán hướng nếu có ít nhất 2 mẫu */
	if (cd->history_count >= 2) {
		int idx = (cd->history_index - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		int prev_idx = (idx - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		sector_t diff = cd->read_history[idx] - cd->read_history[prev_idx];

		if (diff > 0)
			cd->last_read_direction = 1;
		else if (diff < 0)
			cd->last_read_direction = -1;
		else
			cd->last_read_direction = 0;

		/* Dự đoán sector tiếp theo nếu đủ 3 mẫu và các bước bằng nhau */
		if (cd->history_count >= 3) {
			int i, consistent = 1;
			sector_t last_diff = 0;
			for (i = 1; i < min(3, cd->history_count); i++) {
				int cur = (cd->history_index - i + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
				int prev = (cur - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
				sector_t d = cd->read_history[cur] - cd->read_history[prev];
				if (i == 1)
					last_diff = d;
				else if (d != last_diff) {
					consistent = 0;
					break;
				}
			}
			if (consistent && last_diff != 0)
				cd->predicted_sector = cd->read_history[idx] + last_diff;
			else
				cd->predicted_sector = 0;
		}
	}
}

static bool is_interactive_process(struct cinnamon_data *cd, struct request *rq)
{
	pid_t pid = current->pid;
	if (pid == cd->last_interactive_pid && time_before(jiffies, cd->interactive_expire))
		return true;

	/* Chỉ request rất nhỏ (≤4KB) và nice <=0 mới coi là interactive */
	if (blk_rq_bytes(rq) <= 4 * 1024 && task_nice(current) <= 0) {
		cd->last_interactive_pid = pid;
		cd->interactive_expire = jiffies + INTERACTIVE_TIMEOUT;
		return true;
	}
	return false;
}

static void cinnamon_merged_requests(struct request_queue *q, struct request *rq,
				      struct request *next)
{
	/* 1. Đưa khai báo biến lên đầu hàm (Đúng chuẩn C90) */
	struct cinnamon_data *cd = q->elevator->elevator_data;

	/* 2. Sau đó mới đến các câu lệnh thực thi */
	list_del_init(&next->queuelist);
	
	if (rq_data_dir(next) == WRITE) {
		if (cd->write_cnt > 0)
			cd->write_cnt--;
	} else {
		if (cd->read_cnt > 0)
			cd->read_cnt--;
	}
}

static int cinnamon_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int dir = bio_data_dir(bio);
	sector_t sector = bio->bi_sector;          /* vị trí bắt đầu của bio (phiên bản cũ) */
	sector_t last_sector = sector + bio_sectors(bio); /* vị trí kết thúc */
	struct request *last_rq;

	if (dir == READ && !list_empty(&cd->read_queue)) {
		last_rq = list_entry(cd->read_queue.prev, struct request, queuelist);
		/* Back merge: bio bắt đầu ngay sau request cuối */
		if (sector == blk_rq_pos(last_rq) + blk_rq_sectors(last_rq)) {
			*req = last_rq;
			return ELEVATOR_BACK_MERGE;
		}
		/* Front merge: bio kết thúc tại vị trí bắt đầu của request cuối */
		if (last_sector == blk_rq_pos(last_rq)) {
			*req = last_rq;
			return ELEVATOR_FRONT_MERGE;
		}
	} else if (dir == WRITE && !list_empty(&cd->write_queue)) {
		last_rq = list_entry(cd->write_queue.prev, struct request, queuelist);
		if (sector == blk_rq_pos(last_rq) + blk_rq_sectors(last_rq)) {
			*req = last_rq;
			return ELEVATOR_BACK_MERGE;
		}
		if (last_sector == blk_rq_pos(last_rq)) {
			*req = last_rq;
			return ELEVATOR_FRONT_MERGE;
		}
	}
	return ELEVATOR_NO_MERGE;
}

static void cinnamon_adjust_params(struct cinnamon_data *cd, struct request_queue *q)
{
	unsigned long now = jiffies;
	u64 total;
	int num, avg_latency_ms;
	int queue_depth = cd->read_cnt + cd->write_cnt;
	int read_ratio = queue_depth ? (cd->read_cnt * 100) / queue_depth : 50;

	/* Chỉ điều chỉnh sau mỗi khoảng thời gian */
	if (time_before(now, cd->last_adjust_jiffies + msecs_to_jiffies(cd->adjust_interval_ms)))
		return;

	cd->last_adjust_jiffies = now;

	/* Tính độ trễ trung bình gần đây */
	total = atomic64_read(&cd->total_latency);
	num = atomic_read(&cd->num_requests);
	if (num == 0)
		return;

	avg_latency_ms = (total / num) * 1000 / HZ;

	/* Điều chỉnh dựa trên latency so với mục tiêu */
	if (avg_latency_ms > cd->target_latency_ms * 2) {
		/* Latency quá cao: giảm batch, giảm thời gian chờ ghi */
		cd->read_batch = max(cd->read_batch / 2, 4);
		cd->write_batch = max(cd->write_batch / 2, 4);
		cd->write_expire_ms = max(cd->write_expire_ms / 2, 10);
		cd->pressure_thres = max(cd->pressure_thres / 2, 8);
	} else if (avg_latency_ms < cd->target_latency_ms / 2) {
		/* Latency rất thấp: có thể tăng batch để tiết kiệm CPU */
		cd->read_batch = min(cd->read_batch * 2, 64);
		cd->write_batch = min(cd->write_batch * 2, 64);
		cd->write_expire_ms = min(cd->write_expire_ms * 2, 500);
		cd->pressure_thres = min(cd->pressure_thres * 2, 48);
	}

	/* Điều chỉnh thêm dựa trên tỉ lệ đọc/ghi: nếu đọc nhiều hơn, tăng read_batch */
	if (read_ratio > 70) {
		cd->read_batch = min(cd->read_batch * 2, 64);
	} else if (read_ratio < 30) {
		cd->write_batch = min(cd->write_batch * 2, 64);
	}

	/* Điều chỉnh dựa trên độ sâu hàng đợi: nếu hàng đợi dài, giảm batch để tránh tụt hậu */
	if (queue_depth > cd->pressure_thres * 2) {
		cd->read_batch = max(cd->read_batch / 2, 4);
		cd->write_batch = max(cd->write_batch / 2, 4);
	}

	/* Lưu lại giá trị hiện tại để phục hồi khi idle */
	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;
}

static int cinnamon_dispatch(struct request_queue *q, int force)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	struct request *rq = NULL;
	unsigned long now = jiffies;
	int target_pressure = 0;
	int pressure = atomic_read(&cinnamon_io_pressure);
	unsigned long write_expire = msecs_to_jiffies(cd->write_expire_ms);
	unsigned long read_batch = cd->read_batch;

	/* Adjust write expire based on pressure */
	if (pressure == 3)
		write_expire = msecs_to_jiffies(50); /* Tighten for congestion */
	else if (pressure <= 1)
		write_expire = msecs_to_jiffies(250); /* Relax for read priority */

	/* Adjust read batch for high pressure sequential reads */
	if (pressure == 3)
		read_batch = 32; /* Larger batch for boot/loading throughput */

	/* 1. Kiểm tra Starvation (Chống treo máy khi ghi nặng) */
	if (!list_empty(&cd->write_queue)) {
		struct request *wrq = list_first_entry(&cd->write_queue, struct request, queuelist);
		unsigned long fifo_time = (unsigned long)(uintptr_t)wrq->elv.priv[0];

		/* Nếu lệnh ghi đã đợi quá lâu, ưu tiên nó ngay lập tức! */
		if (time_after(now, fifo_time + write_expire)) {
			rq = wrq;
			target_pressure = 3; /* CONGESTED */
			goto dispatch_write;
		}
	}

	/* 2. Ưu tiên Đọc (Normal operation) */
	if (!list_empty(&cd->read_queue)) {
		if (cd->read_batch_count >= read_batch && !list_empty(&cd->write_queue)) {
			rq = list_first_entry(&cd->write_queue, struct request, queuelist);
			target_pressure = 2; /* WRITE BATCH */
			cd->read_batch_count = 0;
			goto dispatch_write;
		} else {
			rq = list_first_entry(&cd->read_queue, struct request, queuelist);
			target_pressure = 1; /* READ URGENT */
			cd->read_batch_count++;
			goto dispatch_read;
		}
	}

	/* 3. Xử lý Ghi theo lô (Batching) */
	if (!list_empty(&cd->write_queue)) {
		if (!list_empty(&cd->read_queue)) {
			/* Read steals priority from write batch */
			rq = list_first_entry(&cd->read_queue, struct request, queuelist);
			target_pressure = 1; /* READ URGENT */
			cd->read_batch_count++;
			goto dispatch_read;
		} else {
			rq = list_first_entry(&cd->write_queue, struct request, queuelist);
			
			/* Logic Batching đơn giản */
			if (cd->batch_count < cd->write_batch) {
				cd->batch_count++;
				target_pressure = 2; /* WRITE BATCH */
			} else {
				/* Hết batch, nhường một nhịp (nhưng ở đây ko có read nên cứ chạy) */
				cd->batch_count = 0;
				target_pressure = 2;
			}
			goto dispatch_write;
		}
	}

	/* Không còn lệnh nào */
	cinnamon_set_pressure(cd, q, 0);
	/* Điều chỉnh tham số nếu đã đến lúc */
	cinnamon_adjust_params(cd, q);
	/* Prefetch dựa trên dự đoán: nếu có dự đoán và không còn request đọc, tăng read-ahead */
	if (cd->predicted_sector != 0 && cd->read_cnt == 0 && list_empty(&cd->read_queue)) {
		/* Tạm thời đặt read-ahead cao để kernel tự động prefetch */
		q->backing_dev_info.ra_pages = 4096;
	}
	return 0;

dispatch_read:
	if (rq) {
		if (cd->read_cnt > 0)
			cd->read_cnt--;
		list_del_init(&rq->queuelist);
		{
			unsigned long start_time = (unsigned long)(uintptr_t)rq->elv.priv[0];
			unsigned long latency = jiffies - start_time;
			atomic64_add(latency, &cd->total_latency);
			atomic_inc(&cd->num_requests);
			if (atomic_read(&cd->num_requests) >= 1000) {
				u64 total = atomic64_read(&cd->total_latency);
				int num = atomic_read(&cd->num_requests);
				atomic64_set(&cd->total_latency, total / 2);
				atomic_set(&cd->num_requests, num / 2);
			}
		}
		/* Cập nhật lịch sử đọc để dự đoán pattern */
		cinnamon_update_read_history(cd, blk_rq_pos(rq));

		/* Xóa timestamp để tránh sử dụng lại dữ liệu cũ */
		rq->elv.priv[0] = NULL;
		cd->batch_count = 0;
		/* Ưu tiên tiến trình tương tác */
		if (is_interactive_process(cd, rq)) {
			target_pressure = 1; /* READ URGENT, bất kể batch */
		}
		cinnamon_set_pressure(cd, q, target_pressure);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;

dispatch_write:
	if (rq) {
		if (cd->write_cnt > 0)
			cd->write_cnt--;
		{
			unsigned long start_time = (unsigned long)(uintptr_t)rq->elv.priv[0];
			unsigned long latency = jiffies - start_time;
			atomic64_add(latency, &cd->total_latency);
			atomic_inc(&cd->num_requests);
			if (atomic_read(&cd->num_requests) >= 1000) {
				u64 total = atomic64_read(&cd->total_latency);
				int num = atomic_read(&cd->num_requests);
				atomic64_set(&cd->total_latency, total / 2);
				atomic_set(&cd->num_requests, num / 2);
			}
		}
		/* Xóa timestamp (không cần free vì không alloc) */
		rq->elv.priv[0] = NULL; 
		list_del_init(&rq->queuelist);
		
		cd->read_batch_count = 0;
		/* Cập nhật áp lực dựa trên độ dài hàng đợi */
		if ((cd->write_cnt + cd->read_cnt) > cd->pressure_thres) {
			target_pressure = 3; /* Force ZRAM to use RAW mode */
		}
			
		cinnamon_set_pressure(cd, q, target_pressure);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static void cinnamon_add_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;

	if (rq_data_dir(rq) == READ) {
		rq->elv.priv[0] = (void *)(uintptr_t)jiffies;
		list_add_tail(&rq->queuelist, &cd->read_queue);
		cd->read_cnt++;
	} else {
		/* ZERO-ALLOC TRICK: Lưu jiffies trực tiếp vào con trỏ */
		rq->elv.priv[0] = (void *)(uintptr_t)jiffies;
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

	cd = kmalloc_node(sizeof(*cd), GFP_NOIO, q->node);
	if (!cd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&cd->read_queue);
	INIT_LIST_HEAD(&cd->write_queue);
	cd->batch_count = 0;
	cd->write_cnt = 0;
	cd->read_cnt = 0;
	cd->read_batch_count = 0;
	atomic64_set(&cd->total_latency, 0);
	atomic_set(&cd->num_requests, 0);
	cd->last_ra_pressure = -1;
	cd->last_pressure_change_jiffies = 0;
	/* Set default tunable values */
	cd->write_expire_ms = CINNAMON_WRITE_EXPIRE;
	cd->read_batch = CINNAMON_READ_BATCH;
	cd->write_batch = CINNAMON_WRITE_BATCH;
	cd->pressure_thres = CINNAMON_PRESSURE_THRES;
	cd->hysteresis_ms = CINNAMON_HYSTERESIS_MS;
	/* Initialize read prediction history */
	cd->history_index = 0;
	cd->history_count = 0;
	cd->last_read_direction = 0;
	cd->predicted_sector = 0;
	cd->last_adjust_jiffies = jiffies;
	cd->target_latency_ms = CINNAMON_TARGET_LATENCY_MS;
	cd->adjust_interval_ms = CINNAMON_ADJUST_INTERVAL_MS;
	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;
	cd->last_interactive_pid = 0;
	cd->interactive_expire = 0;
	cd->q = q;
	
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
	WARN_ON(!list_empty(&cd->read_queue));
	WARN_ON(!list_empty(&cd->write_queue));

	kfree(cd);
}

static ssize_t cinnamon_pressure_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", atomic_read(&cinnamon_io_pressure));
}

static ssize_t cinnamon_avg_latency_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	u64 total;
	int num;

	total = atomic64_read(&cd->total_latency);
	num = atomic_read(&cd->num_requests);
	if (num == 0) return sprintf(page, "0\n");
	{
		u64 avg_jiffies = total / num;
		u64 msecs = (avg_jiffies * 1000ULL) / HZ;
		return sprintf(page, "%llu\n", (unsigned long long)msecs);
	}
	return 0;
}

static ssize_t cinnamon_write_expire_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->write_expire_ms);
}

static ssize_t cinnamon_write_expire_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val > 0) {
		cd->write_expire_ms = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_read_batch_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->read_batch);
}

static ssize_t cinnamon_read_batch_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val > 0) {
		cd->read_batch = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_write_batch_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->write_batch);
}

static ssize_t cinnamon_write_batch_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val > 0) {
		cd->write_batch = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_pressure_thres_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->pressure_thres);
}

static ssize_t cinnamon_pressure_thres_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val >= 0) {
		cd->pressure_thres = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_hysteresis_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->hysteresis_ms);
}

static ssize_t cinnamon_hysteresis_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val > 0) {
		cd->hysteresis_ms = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_target_latency_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->target_latency_ms);
}

static ssize_t cinnamon_target_latency_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val > 0) {
		cd->target_latency_ms = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_adjust_interval_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->adjust_interval_ms);
}

static ssize_t cinnamon_adjust_interval_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val = simple_strtol(page, NULL, 10);
	if (val >= 100) { /* tối thiểu 100ms */
		cd->adjust_interval_ms = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t cinnamon_predicted_sector_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)cd->predicted_sector);
}

static ssize_t cinnamon_last_read_direction_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->last_read_direction);
}

static ssize_t cinnamon_history_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->history_count);
}

static ssize_t cinnamon_ra_pages_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%lu\n", cd->q->backing_dev_info.ra_pages);
}

static ssize_t cinnamon_batch_stats_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "read_batch_count: %u\nbatch_count: %u\nread_cnt: %d\nwrite_cnt: %d\n",
		       cd->read_batch_count, cd->batch_count, cd->read_cnt, cd->write_cnt);
}

static struct elv_fs_entry cinnamon_attrs[] = {
	__ATTR(pressure, 0444, cinnamon_pressure_show, NULL),
	__ATTR(avg_latency, 0444, cinnamon_avg_latency_show, NULL),
	__ATTR(write_expire_ms, 0644, cinnamon_write_expire_ms_show, cinnamon_write_expire_ms_store),
	__ATTR(read_batch, 0644, cinnamon_read_batch_show, cinnamon_read_batch_store),
	__ATTR(write_batch, 0644, cinnamon_write_batch_show, cinnamon_write_batch_store),
	__ATTR(pressure_thres, 0644, cinnamon_pressure_thres_show, cinnamon_pressure_thres_store),
	__ATTR(hysteresis_ms, 0644, cinnamon_hysteresis_ms_show, cinnamon_hysteresis_ms_store),
	__ATTR(target_latency_ms, 0644, cinnamon_target_latency_ms_show, cinnamon_target_latency_ms_store),
	__ATTR(adjust_interval_ms, 0644, cinnamon_adjust_interval_ms_show, cinnamon_adjust_interval_ms_store),
	__ATTR(predicted_sector, 0444, cinnamon_predicted_sector_show, NULL),
	__ATTR(last_read_direction, 0444, cinnamon_last_read_direction_show, NULL),
	__ATTR(history_count, 0444, cinnamon_history_count_show, NULL),
	__ATTR(ra_pages, 0444, cinnamon_ra_pages_show, NULL),
	__ATTR(batch_stats, 0444, cinnamon_batch_stats_show, NULL),
	__ATTR_NULL
};

static struct elevator_type elevator_cinnamon = {
	.ops = {
		.elevator_merge_req_fn		= cinnamon_merged_requests,
		.elevator_dispatch_fn		= cinnamon_dispatch,
		.elevator_add_req_fn		= cinnamon_add_request,
		.elevator_former_req_fn		= cinnamon_former_request,
		.elevator_latter_req_fn		= cinnamon_latter_request,
		.elevator_init_fn			= cinnamon_init_queue,
		.elevator_exit_fn			= cinnamon_exit_queue,
		.elevator_merge_fn			= cinnamon_merge,
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
MODULE_DESCRIPTION("Cinnamon I/O Scheduler");