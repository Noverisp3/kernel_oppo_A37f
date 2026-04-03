/*
 * cinnamon-iosched.c - Cinnamon I/O Scheduler with Atomic-Stream Alignment (ASA)
 *
 * A multi‑priority I/O scheduler designed for flash storage.
 * Features: tiered queues, adaptive tuning, read‑ahead smoothing, starvation
 * avoidance, and a coalescing buffer for small writes.
 *
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/backing-dev.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/genhd.h>

/* ============================================================================
 * Tunables (can be modified via sysfs)
 * ============================================================================
 */
#define CINNAMON_WRITE_EXPIRE        (HZ / 8)      /* 125 ms */
#define CINNAMON_READ_BATCH          8
#define CINNAMON_WRITE_BATCH         16
#define CINNAMON_PRESSURE_THRES      24
#define CINNAMON_HYSTERESIS_MS       500           /* 500 ms */
#define CINNAMON_HISTORY_SIZE        8
#define CINNAMON_TARGET_LATENCY_MS   50
#define CINNAMON_ADJUST_INTERVAL_MS  2000          /* 2 s */
#define WRITE_STARVE_THRESH_NS       150000000ULL  /* 150 ms */

/* Starvation recovery */
#define STARVATION_RECOVERY_DURATION  (2 * HZ)     /* 2 s */
#define STARVATION_RECOVERY_STEPS     4
#define STARVATION_MIN_READ_BATCH     4
#define STARVATION_RECOVERY_INTERVAL  (HZ / 2)     /* 500 ms */
#define STARVATION_MAX_EVENTS         3

/* Merge & history */
#define MAX_MERGE_SCAN                8
#define CINNAMON_HISTORY_UPDATE_INTERVAL 8
#define FG_STARVE_LIMIT               16

/* ASA (Atomic‑Stream Alignment) */
#define ASA_SMALL_WRITE_SIZE          64          /* 64 KB */
#define ASA_ENHANCED_MERGE_SCAN       24
#define ASA_WRITE_EXPIRE_MS           200
#define ASA_COALESCE_THRESHOLD        4
#define ASA_MAX_COALESCE_TIME_MS      300

/* Coalescing buffer */
#define ASA_COALESCING_BUFFER         1
#define ASA_MAX_COALESCE_SIZE         256         /* 256 KB */
#define ASA_COALESCE_BUFFER_ENTRIES   16
#define ASA_COALESCE_FLUSH_MS         150

/* Sector‑sorted queues */
#define SECTOR_SORTED_INSERT          1
#define ADAPTIVE_MERGE_SCAN           1
#define DEFAULT_MERGE_SCAN            16
#define MAX_ADAPTIVE_MERGE_SCAN       32
#define SECTOR_PROXIMITY_THRESHOLD    1024

/* Proactive tuning */
#define PROACTIVE_TUNING              1
#define DISPATCH_TUNING_INTERVAL      64
#define PERIODIC_TUNING_INTERVAL_MS   2000
#define MIN_TUNING_CHANGE_THRESHOLD   10
#define URGENT_TUNING_THRESHOLD       150

/* Real‑time pressure updates */
#define REALTIME_PRESSURE_UPDATES     1
#define PRESSURE_UPDATE_THROTTLE_MS   100
#define PRESSURE_UPDATE_INTERVAL      8

/* eMMC specific */
#define EMMC_MAJOR                    179
#define EMMC_MAX_BATCH                16
#define EMMC_MIN_PRESSURE_THRES       12
#define EMMC_MAX_PRESSURE_THRES       24

/* Priority levels */
enum io_priority {
	PRIO_CRITICAL = 0,
	PRIO_TRIM,
	PRIO_READ_FG,
	PRIO_WRITE_FG,
	PRIO_READ_BG,
	PRIO_WRITE_BG,
	PRIO_COUNT
};

/* Storing timestamp + priority in elv.priv */
#define CINNAMON_PRIO_SHIFT 8
#define CINNAMON_PRIO_MASK  ((1 << CINNAMON_PRIO_SHIFT) - 1)

/* ============================================================================
 * Data structures
 * ============================================================================
 */
struct cinnamon_data {
	/* Tiered queues */
	struct list_head queues[PRIO_COUNT];
	unsigned int queue_counts[PRIO_COUNT];

	/* Batching state */
	unsigned int read_batch_count;
	unsigned int write_batch_count;
	unsigned int fg_dispatch_count;

	/* Latency tracking (ns) */
	atomic64_t total_latency_ns;
	atomic_t num_requests;

	/* Pressure & read‑ahead */
	int last_ra_pressure;
	unsigned long last_pressure_change_jiffies;
	unsigned long last_manual_param_change_jiffies;

	/* Tunable parameters */
	int write_expire_ms;
	int read_batch;
	int write_batch;
	int pressure_thres;
	int hysteresis_ms;

	/* Read pattern history */
	sector_t read_history[CINNAMON_HISTORY_SIZE];
	int history_index;
	int history_count;
	int last_read_direction;
	sector_t predicted_sector;

	/* Write pattern history */
	sector_t write_history[CINNAMON_HISTORY_SIZE];
	int write_history_index;
	int write_history_count;
	int last_write_direction;
	sector_t predicted_write_sector;

	/* Adaptive tuning */
	unsigned long last_adjust_jiffies;
	int target_latency_ms;
	int adjust_interval_ms;

	/* Saved parameters for idle mode */
	int saved_read_batch;
	int saved_write_batch;
	int saved_write_expire_ms;

	struct request_queue *q;
	int dispatch_seq;
	int empty_dispatch_count;
	bool emmc_mode;

	/* Starvation auto‑recovery */
	unsigned int write_starved_count;
	unsigned long last_starved_jiffies;
	unsigned long starvation_recovery_until;
	int saved_read_batch_for_starve;
	unsigned int starvation_recovery_steps;
	unsigned int starvation_events_count;

	/* Sequential write boost */
	unsigned int seq_write_count;
	sector_t last_write_sector;

	/* ASA fields */
	unsigned int small_write_count;
	unsigned long asa_last_small_write_jiffies;
	unsigned int asa_merge_count;
	unsigned int asa_coalesce_count;
	sector_t asa_last_sector;
	bool asa_active;

	/* Proactive tuning fields */
	unsigned long last_tuning_jiffies;
	unsigned int dispatch_count_since_tuning;
	int last_avg_latency_ms;
	bool urgent_tuning_mode;
	unsigned long periodic_tuning_jiffies;

	/* Real‑time pressure fields */
	unsigned long last_pressure_update_jiffies;
	unsigned int requests_since_pressure_update;
	int last_calculated_pressure;

	/* ASA coalescing buffer */
	struct list_head asa_coalesce_buffer;
	unsigned int asa_buffer_count;
	unsigned int asa_buffer_sectors;
	unsigned long asa_buffer_start_time;
	sector_t asa_buffer_start_sector;
	bool asa_buffer_active;
	unsigned int asa_coalesced_requests;
	unsigned int asa_buffer_flushes;
};

/* Global pressure indicator */
atomic_t cinnamon_io_pressure = ATOMIC_INIT(0);
EXPORT_SYMBOL(cinnamon_io_pressure);

/* ============================================================================
 * Timestamp & priority helpers
 * ============================================================================
 */
static inline void cinnamon_set_req_time_prio(struct request *rq,
					      u64 time_ns, int prio)
{
	u64 val = (time_ns << CINNAMON_PRIO_SHIFT) | (prio & CINNAMON_PRIO_MASK);
#if BITS_PER_LONG == 64
	rq->elv.priv[0] = (void *)val;
#else
	rq->elv.priv[0] = (void *)(unsigned long)(val & 0xFFFFFFFF);
	rq->elv.priv[1] = (void *)(unsigned long)(val >> 32);
#endif
}

static inline u64 cinnamon_get_req_time(struct request *rq)
{
	u64 val;
#if BITS_PER_LONG == 64
	val = (u64)(unsigned long)rq->elv.priv[0];
#else
	u64 low = (u64)(unsigned long)rq->elv.priv[0];
	u64 high = (u64)(unsigned long)rq->elv.priv[1];
	val = (high << 32) | low;
#endif
	return val >> CINNAMON_PRIO_SHIFT;
}

static inline int cinnamon_get_req_prio(struct request *rq)
{
	u64 val;
#if BITS_PER_LONG == 64
	val = (u64)(unsigned long)rq->elv.priv[0];
#else
	u64 low = (u64)(unsigned long)rq->elv.priv[0];
	u64 high = (u64)(unsigned long)rq->elv.priv[1];
	val = (high << 32) | low;
#endif
	return val & CINNAMON_PRIO_MASK;
}

/* ============================================================================
 * eMMC & pressure helpers
 * ============================================================================
 */
static void cinnamon_clamp_for_emmc(struct cinnamon_data *cd)
{
	if (!cd->emmc_mode)
		return;
	cd->read_batch = min(cd->read_batch, EMMC_MAX_BATCH);
	cd->write_batch = min(cd->write_batch, EMMC_MAX_BATCH);
	cd->pressure_thres = clamp(cd->pressure_thres,
				   EMMC_MIN_PRESSURE_THRES,
				   EMMC_MAX_PRESSURE_THRES);
}

static void cinnamon_update_ra(struct cinnamon_data *cd)
{
	int curr_press = atomic_read(&cinnamon_io_pressure);
	unsigned int ra_kb;
	unsigned long target_ra_pages;
	unsigned long current_ra;
	
	if (curr_press == cd->last_ra_pressure)
		return;

	switch (curr_press) {
	case 3: ra_kb = 128; break;
	case 2: ra_kb = 512; break;
	case 1: ra_kb = 1024; break;
	default: ra_kb = 2048; break;
	}
	target_ra_pages = ra_kb >> (PAGE_SHIFT - 10);
	current_ra = cd->q->backing_dev_info.ra_pages;

	if (current_ra < target_ra_pages)
		current_ra = min(current_ra + 16, target_ra_pages);
	else if (current_ra > target_ra_pages)
		current_ra = max(current_ra - 16, target_ra_pages);

	cd->q->backing_dev_info.ra_pages = current_ra;
	cd->last_ra_pressure = curr_press;
}

static void cinnamon_set_pressure(struct cinnamon_data *cd, int new_pressure)
{
	int curr_pressure, old_pressure;
	int changed = 0;

	do {
		curr_pressure = atomic_read(&cinnamon_io_pressure);
		if (new_pressure == curr_pressure)
			return;

		if (new_pressure == 0) {
			old_pressure = atomic_cmpxchg(&cinnamon_io_pressure,
						       curr_pressure, 0);
			if (old_pressure == curr_pressure) {
				cd->last_pressure_change_jiffies = jiffies;
				changed = 1;
				if (curr_pressure != 0) {
					cd->read_batch = cd->saved_read_batch;
					cd->write_batch = cd->saved_write_batch;
					cd->write_expire_ms = cd->saved_write_expire_ms;
				}
			}
			break;
		}

		if (!time_after(jiffies, cd->last_pressure_change_jiffies +
				msecs_to_jiffies(cd->hysteresis_ms)))
			return;

		if (curr_pressure == 0 || new_pressure > curr_pressure) {
			old_pressure = atomic_cmpxchg(&cinnamon_io_pressure,
						       curr_pressure, new_pressure);
			if (old_pressure == curr_pressure) {
				cd->last_pressure_change_jiffies = jiffies;
				changed = 1;

				if (new_pressure == 0) {
					cd->saved_read_batch = cd->read_batch;
					cd->saved_write_batch = cd->write_batch;
					cd->saved_write_expire_ms = cd->write_expire_ms;
					cd->read_batch = max(cd->read_batch / 2, 8);
					cd->write_batch = max(cd->write_batch / 2, 8);
					cd->write_expire_ms = min(cd->write_expire_ms + 100, 300);
				} else if (curr_pressure == 0) {
					cd->read_batch = cd->saved_read_batch;
					cd->write_batch = cd->saved_write_batch;
					cd->write_expire_ms = cd->saved_write_expire_ms;
				}
				cinnamon_clamp_for_emmc(cd);
			}
		} else {
			return; /* cannot decrease except to idle */
		}
	} while (!changed);

	if (changed)
		cinnamon_update_ra(cd);
}

/* ============================================================================
 * Pattern history (read/write)
 * ============================================================================
 */
static void cinnamon_update_read_history(struct cinnamon_data *cd, sector_t sector)
{
	cd->read_history[cd->history_index] = sector;
	cd->history_index = (cd->history_index + 1) % CINNAMON_HISTORY_SIZE;
	if (cd->history_count < CINNAMON_HISTORY_SIZE)
		cd->history_count++;

	if (cd->history_count >= 3) {
		int i, consistent = 0, total_diffs = 0;
		sector_t avg_diff = 0, min_diff = 0, max_diff = 0;
		int idx = (cd->history_index - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		int confidence;

		for (i = 1; i < min(5, cd->history_count); i++) {
			int cur = (cd->history_index - i + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			int prev = (cur - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			sector_t diff = cd->read_history[cur] - cd->read_history[prev];

			if (i == 1) {
				min_diff = max_diff = diff;
				avg_diff = diff;
			} else {
				min_diff = min(min_diff, diff);
				max_diff = max(max_diff, diff);
				avg_diff = (avg_diff * (i-1) + diff) / i;
			}
			total_diffs++;
			if (abs(diff - avg_diff) < 1024)
				consistent++;
		}

		confidence = consistent * 100 / total_diffs;
		if (confidence >= 80 && abs(max_diff - min_diff) < 2048 &&
		    avg_diff != 0 && abs(avg_diff) < 1048576) {
			cd->predicted_sector = cd->read_history[idx] + avg_diff;
			cd->last_read_direction = avg_diff > 0 ? 1 : -1;
		} else {
			cd->predicted_sector = 0;
			cd->last_read_direction = 0;
		}
	} else {
		cd->predicted_sector = 0;
		cd->last_read_direction = 0;
	}
}

static void cinnamon_update_write_history(struct cinnamon_data *cd, sector_t sector)
{
	cd->write_history[cd->write_history_index] = sector;
	cd->write_history_index = (cd->write_history_index + 1) % CINNAMON_HISTORY_SIZE;
	if (cd->write_history_count < CINNAMON_HISTORY_SIZE)
		cd->write_history_count++;

	if (cd->write_history_count >= 3) {
		int i, consistent = 0, total_diffs = 0;
		sector_t avg_diff = 0, min_diff = 0, max_diff = 0;
		int idx = (cd->write_history_index - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		int confidence;

		for (i = 1; i < min(5, cd->write_history_count); i++) {
			int cur = (cd->write_history_index - i + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			int prev = (cur - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			sector_t diff = cd->write_history[cur] - cd->write_history[prev];

			if (i == 1) {
				min_diff = max_diff = diff;
				avg_diff = diff;
			} else {
				min_diff = min(min_diff, diff);
				max_diff = max(max_diff, diff);
				avg_diff = (avg_diff * (i-1) + diff) / i;
			}
			total_diffs++;
			if (abs(diff - avg_diff) < 1024)
				consistent++;
		}

		confidence = consistent * 100 / total_diffs;
		if (confidence >= 80 && abs(max_diff - min_diff) < 2048 &&
		    avg_diff != 0 && abs(avg_diff) < 1048576) {
			cd->predicted_write_sector = cd->write_history[idx] + avg_diff;
			cd->last_write_direction = avg_diff > 0 ? 1 : -1;
		} else {
			cd->predicted_write_sector = 0;
			cd->last_write_direction = 0;
		}
	} else {
		cd->predicted_write_sector = 0;
		cd->last_write_direction = 0;
	}

	/* Sequential write detection for eMMC boost */
	if (cd->last_write_direction != 0 && cd->last_write_sector != 0) {
		if (sector == cd->last_write_sector + 1)
			cd->seq_write_count++;
		else
			cd->seq_write_count = 0;
	}
	cd->last_write_sector = sector;
}

/* ============================================================================
 * Proactive tuning helpers
 * ============================================================================
 */
static bool cinnamon_should_tune_now(struct cinnamon_data *cd, int current_avg_latency_ms)
{
	unsigned long now = jiffies;
	int change = 0;
	
	if (!PROACTIVE_TUNING)
		return false;

	if (cd->dispatch_count_since_tuning >= DISPATCH_TUNING_INTERVAL)
		return true;

	if (time_after(now, cd->periodic_tuning_jiffies +
		       msecs_to_jiffies(PERIODIC_TUNING_INTERVAL_MS)))
		return true;

	if (current_avg_latency_ms > 0) {
		if (cd->last_avg_latency_ms > 0) {
			change = abs(current_avg_latency_ms - cd->last_avg_latency_ms) * 100 /
				 cd->last_avg_latency_ms;
		}
		if (current_avg_latency_ms > (cd->target_latency_ms *
					      URGENT_TUNING_THRESHOLD / 100)) {
			cd->urgent_tuning_mode = true;
			return true;
		}
		if (change >= MIN_TUNING_CHANGE_THRESHOLD)
			return true;
	}
	return false;
}

static void cinnamon_reset_tuning_counters(struct cinnamon_data *cd,
					   int current_avg_latency_ms)
{
	unsigned long now = jiffies;
	cd->last_tuning_jiffies = now;
	cd->dispatch_count_since_tuning = 0;
	cd->last_avg_latency_ms = current_avg_latency_ms;
	cd->periodic_tuning_jiffies = now;
	cd->urgent_tuning_mode = false;
}

static inline void cinnamon_update_tuning_counters(struct cinnamon_data *cd)
{
	cd->dispatch_count_since_tuning++;
}

/* ============================================================================
 * Real‑time pressure helpers
 * ============================================================================
 */
static int cinnamon_calc_pressure_from_queue_depth(struct cinnamon_data *cd)
{
	int total = 0, i;
	int pressure;
	
	for (i = 0; i < PRIO_COUNT; i++)
		total += cd->queue_counts[i];

	if (total == 0)
		pressure = 0;
	else if (total <= cd->pressure_thres / 2)
		pressure = 1;
	else if (total <= cd->pressure_thres)
		pressure = 2;
	else
		pressure = 3;

	if (cd->queue_counts[PRIO_CRITICAL] > 4 ||
	    cd->queue_counts[PRIO_TRIM] > 8)
		pressure = min(pressure + 1, 3);

	return pressure;
}

static bool cinnamon_should_update_pressure_now(struct cinnamon_data *cd,
						int new_pressure)
{
	unsigned long now = jiffies;
	if (!REALTIME_PRESSURE_UPDATES)
		return false;
	if (new_pressure != cd->last_calculated_pressure)
		return true;
	if (time_after(now, cd->last_pressure_update_jiffies +
		       msecs_to_jiffies(PRESSURE_UPDATE_THROTTLE_MS)))
		return true;
	if (cd->requests_since_pressure_update >= PRESSURE_UPDATE_INTERVAL)
		return true;
	return false;
}

static void cinnamon_update_pressure_on_add(struct cinnamon_data *cd)
{
	int new_pressure;
	
	if (!REALTIME_PRESSURE_UPDATES)
		return;

	new_pressure = cinnamon_calc_pressure_from_queue_depth(cd);
	if (cinnamon_should_update_pressure_now(cd, new_pressure)) {
		cinnamon_set_pressure(cd, new_pressure);
		cd->last_pressure_update_jiffies = jiffies;
		cd->requests_since_pressure_update = 0;
		cd->last_calculated_pressure = new_pressure;
	}
	cd->requests_since_pressure_update++;
}

/* ============================================================================
 * Starvation recovery (progressive)
 * ============================================================================
 */
static void cinnamon_initiate_starvation_recovery(struct cinnamon_data *cd)
{
	unsigned long now = jiffies;
	if (!cd->starvation_recovery_until) {
		cd->saved_read_batch_for_starve = cd->read_batch;
		cd->starvation_events_count++;
	}
	cd->starvation_recovery_until = now + STARVATION_RECOVERY_DURATION;
	cd->starvation_recovery_steps = 0;
	cd->read_batch = STARVATION_MIN_READ_BATCH;
	cinnamon_clamp_for_emmc(cd);
}

static void cinnamon_progressive_recovery_step(struct cinnamon_data *cd)
{
	unsigned long now = jiffies;
	unsigned long step_time;
	int target, curr_batch, inc;
	
	if (!cd->starvation_recovery_until)
		return;

	step_time = cd->starvation_recovery_until -
				  STARVATION_RECOVERY_DURATION;
	cd->starvation_recovery_steps = (now - step_time) /
					STARVATION_RECOVERY_INTERVAL;

	if (cd->starvation_recovery_steps >= STARVATION_RECOVERY_STEPS) {
		cd->read_batch = cd->saved_read_batch_for_starve;
		cd->starvation_recovery_until = 0;
		cd->starvation_recovery_steps = 0;
		cinnamon_clamp_for_emmc(cd);
		return;
	}

	target = cd->saved_read_batch_for_starve;
	curr_batch = cd->read_batch;
	inc = (target - curr_batch) /
		  (STARVATION_RECOVERY_STEPS - cd->starvation_recovery_steps);
	if (inc > 0) {
		cd->read_batch = curr_batch + inc;
		cinnamon_clamp_for_emmc(cd);
	}
}

/* ============================================================================
 * ASA coalescing buffer
 * ============================================================================
 */
static bool cinnamon_should_coalesce_request(struct request *rq)
{
	if (!ASA_COALESCING_BUFFER)
		return false;
	if (rq_data_dir(rq) != WRITE)
		return false;
	if (rq->cmd_flags & (REQ_SYNC | REQ_DISCARD | REQ_META | REQ_PRIO))
		return false;
	return blk_rq_sectors(rq) <= ASA_SMALL_WRITE_SIZE;
}

static bool cinnamon_can_add_to_buffer(struct cinnamon_data *cd,
				       struct request *rq)
{
	sector_t rq_sector = blk_rq_pos(rq);
	if (!cd->asa_buffer_active)
		return true;
	/* Check sequential */
	if (rq_sector == cd->asa_buffer_start_sector + cd->asa_buffer_sectors) {
		if (cd->asa_buffer_sectors + blk_rq_sectors(rq) <= ASA_MAX_COALESCE_SIZE)
			return true;
	}
	return false;
}

static void cinnamon_add_to_coalesce_buffer(struct cinnamon_data *cd,
					    struct request *rq)
{
	unsigned long now = jiffies;
	if (!cd->asa_buffer_active) {
		INIT_LIST_HEAD(&cd->asa_coalesce_buffer);
		cd->asa_buffer_count = 0;
		cd->asa_buffer_sectors = 0;
		cd->asa_buffer_start_time = now;
		cd->asa_buffer_start_sector = blk_rq_pos(rq);
		cd->asa_buffer_active = true;
	}
	list_add_tail(&rq->queuelist, &cd->asa_coalesce_buffer);
	cd->asa_buffer_count++;
	cd->asa_buffer_sectors += blk_rq_sectors(rq);
	cd->asa_coalesced_requests++;
}

static bool cinnamon_should_flush_coalesce_buffer(struct cinnamon_data *cd)
{
	if (!cd->asa_buffer_active)
		return false;
	if (cd->asa_buffer_count >= ASA_COALESCE_BUFFER_ENTRIES)
		return true;
	if (cd->asa_buffer_sectors >= ASA_MAX_COALESCE_SIZE)
		return true;
	if (time_after(jiffies, cd->asa_buffer_start_time +
		       msecs_to_jiffies(ASA_COALESCE_FLUSH_MS)))
		return true;
	return false;
}

static void cinnamon_flush_coalesce_buffer(struct request_queue *q)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	struct request *rq, *tmp;
	struct request *first_rq;
	sector_t start;
	unsigned int total;
	
	if (!cd->asa_buffer_active || list_empty(&cd->asa_coalesce_buffer))
		return;

	first_rq = list_first_entry(&cd->asa_coalesce_buffer,
				     struct request, queuelist);
	start = cd->asa_buffer_start_sector;
	total = cd->asa_buffer_sectors;

	/* Merge all buffered requests into one */
	list_for_each_entry_safe(rq, tmp, &cd->asa_coalesce_buffer, queuelist) {
		list_del_init(&rq->queuelist);
		/* Override the first request to cover the whole range */
		if (rq == first_rq) {
			rq->__sector = start;
			rq->__data_len = total << 9;
			if (rq->bio) {
				rq->bio->bi_sector = start;
				rq->bio->bi_size = total << 9;
			}
			list_add(&rq->queuelist, &cd->queues[PRIO_WRITE_FG]);
			cd->queue_counts[PRIO_WRITE_FG]++;
		} else {
			blk_put_request(rq);
		}
	}

	cd->asa_buffer_active = false;
	cd->asa_buffer_count = 0;
	cd->asa_buffer_sectors = 0;
	cd->asa_buffer_flushes++;
}

/* ============================================================================
 * Merge helpers (sector‑sorted, adaptive scan)
 * ============================================================================
 */
static bool cinnamon_is_small_write(struct request *rq)
{
	return (rq_data_dir(rq) == WRITE &&
		blk_rq_sectors(rq) <= ASA_SMALL_WRITE_SIZE);
}

static void cinnamon_insert_sorted_by_sector(struct list_head *queue_head,
					     struct request *rq)
{
	sector_t rq_sector = blk_rq_pos(rq);
	struct request *pos;

	if (rq->cmd_flags & REQ_SYNC) {
		list_add(&rq->queuelist, queue_head);
		return;
	}

	list_for_each_entry(pos, queue_head, queuelist) {
		if (rq_sector < blk_rq_pos(pos)) {
			list_add_tail(&rq->queuelist, &pos->queuelist);
			return;
		}
	}
	list_add_tail(&rq->queuelist, queue_head);
}

static int cinnamon_get_adaptive_merge_scan(struct cinnamon_data *cd, int prio,
					    sector_t target_sector)
{
	int queue_depth = cd->queue_counts[prio];
	int base_scan = DEFAULT_MERGE_SCAN;
	int max_scan = MAX_ADAPTIVE_MERGE_SCAN;
	struct request *pos;
	int proximity;

	if (!ADAPTIVE_MERGE_SCAN)
		return base_scan;

	if (queue_depth > 16)
		base_scan = min(base_scan * 2, max_scan);

	if (cd->asa_active && cd->small_write_count >= ASA_COALESCE_THRESHOLD)
		base_scan = max(base_scan, ASA_ENHANCED_MERGE_SCAN);

	if (target_sector) {
		proximity = 0;
		list_for_each_entry(pos, &cd->queues[prio], queuelist) {
			if (abs(blk_rq_pos(pos) - target_sector) <= SECTOR_PROXIMITY_THRESHOLD)
				proximity++;
			if (proximity >= 4) {
				base_scan = min(base_scan + 8, max_scan);
				break;
			}
		}
	}
	return base_scan;
}

static int cinnamon_merge(struct request_queue *q, struct request **req,
			  struct bio *bio)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int dir = bio_data_dir(bio);
	sector_t sector = bio->bi_sector;
	sector_t last_sector = sector + bio_sectors(bio);
	struct list_head *queue_head;
	int target_prio = -1;
	int scanned = 0;
	int max_scan;
	struct request *rq_iter;
	sector_t rq_sector, rq_last;

	if (dir == READ) {
		if (!list_empty(&cd->queues[PRIO_READ_FG]))
			queue_head = &cd->queues[PRIO_READ_FG], target_prio = PRIO_READ_FG;
		else if (!list_empty(&cd->queues[PRIO_READ_BG]))
			queue_head = &cd->queues[PRIO_READ_BG], target_prio = PRIO_READ_BG;
		else
			return ELEVATOR_NO_MERGE;
	} else {
		if (!list_empty(&cd->queues[PRIO_WRITE_FG]))
			queue_head = &cd->queues[PRIO_WRITE_FG], target_prio = PRIO_WRITE_FG;
		else if (!list_empty(&cd->queues[PRIO_WRITE_BG]))
			queue_head = &cd->queues[PRIO_WRITE_BG], target_prio = PRIO_WRITE_BG;
		else
			return ELEVATOR_NO_MERGE;
	}

	max_scan = cinnamon_get_adaptive_merge_scan(cd, target_prio, sector);

	list_for_each_entry(rq_iter, queue_head, queuelist) {
		rq_sector = blk_rq_pos(rq_iter);
		rq_last = rq_sector + blk_rq_sectors(rq_iter);

		if (sector == rq_last) {
			*req = rq_iter;
			if (max_scan >= ASA_ENHANCED_MERGE_SCAN) {
				cd->asa_merge_count++;
				cd->asa_active = true;
			}
			return ELEVATOR_BACK_MERGE;
		}
		if (last_sector == rq_sector) {
			*req = rq_iter;
			if (max_scan >= ASA_ENHANCED_MERGE_SCAN) {
				cd->asa_merge_count++;
				cd->asa_active = true;
			}
			return ELEVATOR_FRONT_MERGE;
		}

		if (SECTOR_SORTED_INSERT && scanned > 8 &&
		    abs(rq_sector - sector) > SECTOR_PROXIMITY_THRESHOLD * 4)
			break; /* too far, unlikely to find more merges */

		if (++scanned >= max_scan)
			break;
	}
	return ELEVATOR_NO_MERGE;
}

static void cinnamon_merged_requests(struct request_queue *q, struct request *rq,
				     struct request *next)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = cinnamon_get_req_prio(next);
	if (prio >= 0 && prio < PRIO_COUNT && cd->queue_counts[prio] > 0)
		cd->queue_counts[prio]--;
	list_del_init(&next->queuelist);
}

/* ============================================================================
 * Add request (with coalescing and sorted insertion)
 * ============================================================================
 */
static void cinnamon_add_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio;
	bool is_small_write;
	sector_t cur;
	int dir;
	
	if (rq->cmd_flags & REQ_DISCARD) {
		prio = PRIO_TRIM;
	} else if (rq->cmd_flags & (REQ_META | REQ_PRIO)) {
		prio = PRIO_CRITICAL;
	} else {
		dir = rq_data_dir(rq);
		if (rq->cmd_flags & REQ_SYNC)
			prio = (dir == READ) ? PRIO_READ_FG : PRIO_WRITE_FG;
		else
			prio = (dir == READ) ? PRIO_READ_BG : PRIO_WRITE_BG;
	}

	/* ASA small write detection */
	is_small_write = cinnamon_is_small_write(rq);
	if (is_small_write) {
		cur = blk_rq_pos(rq);
		if (cd->asa_last_sector &&
		    (cur == cd->asa_last_sector + 1 ||
		     cur + blk_rq_sectors(rq) == cd->asa_last_sector))
			cd->asa_coalesce_count++;

		cd->small_write_count++;
		cd->asa_last_small_write_jiffies = jiffies;
		cd->asa_last_sector = cur;

		if (cd->small_write_count >= ASA_COALESCE_THRESHOLD)
			cd->asa_active = true;
	}

	/* Try coalescing buffer */
	if (cinnamon_should_coalesce_request(rq) &&
	    cinnamon_can_add_to_buffer(cd, rq)) {
		cinnamon_add_to_coalesce_buffer(cd, rq);
		if (cinnamon_should_flush_coalesce_buffer(cd))
			cinnamon_flush_coalesce_buffer(q);
		return;
	} else if (cd->asa_buffer_active) {
		cinnamon_flush_coalesce_buffer(q);
	}

	cinnamon_set_req_time_prio(rq, ktime_to_ns(ktime_get()), prio);

	if (SECTOR_SORTED_INSERT)
		cinnamon_insert_sorted_by_sector(&cd->queues[prio], rq);
	else if (rq->cmd_flags & REQ_SYNC)
		list_add(&rq->queuelist, &cd->queues[prio]);
	else
		list_add_tail(&rq->queuelist, &cd->queues[prio]);

	cd->queue_counts[prio]++;

	cinnamon_update_pressure_on_add(cd);
}

/* ============================================================================
 * Latency moving average
 * ============================================================================
 */
static void cinnamon_update_latency(struct cinnamon_data *cd, u64 latency_ns)
{
	u64 old_avg = atomic64_read(&cd->total_latency_ns);
	int old_count = atomic_read(&cd->num_requests);

	if (old_count > 0)
		atomic64_set(&cd->total_latency_ns,
			     (old_avg * 7 + latency_ns * 3) / 10);
	else
		atomic64_set(&cd->total_latency_ns, latency_ns);

	if (old_count < 100)
		atomic_set(&cd->num_requests, old_count + 1);
	else
		atomic_set(&cd->num_requests, old_count / 2);
}

/* ============================================================================
 * Adaptive parameter tuning
 * ============================================================================
 */
static void cinnamon_adjust_params(struct cinnamon_data *cd)
{
	unsigned long now = jiffies;
	u64 total_ns;
	int num;
	int avg_latency_ms;
	int queue_depth, i;
	bool sig_change;
	int read_cnt, write_cnt, total_io;
	int read_ratio;
	
	if (time_before(now, cd->last_manual_param_change_jiffies +
			msecs_to_jiffies(5000)))
		return;

	if (!cd->urgent_tuning_mode &&
	    time_before(now, cd->last_adjust_jiffies +
			msecs_to_jiffies(cd->adjust_interval_ms)))
		return;

	cd->last_adjust_jiffies = now;

	total_ns = atomic64_read(&cd->total_latency_ns);
	num = atomic_read(&cd->num_requests);
	if (!num)
		return;
	avg_latency_ms = div64_u64(total_ns / num, 1000000);

	queue_depth = 0;
	for (i = 0; i < PRIO_COUNT; i++)
		queue_depth += cd->queue_counts[i];

	sig_change = false;
	if (cd->last_avg_latency_ms > 0) {
		int change = abs(avg_latency_ms - cd->last_avg_latency_ms) * 100 /
			     cd->last_avg_latency_ms;
		sig_change = (change >= MIN_TUNING_CHANGE_THRESHOLD);
	}

	/* Tuning aggressiveness */
	if (cd->urgent_tuning_mode || sig_change) {
		if (avg_latency_ms > cd->target_latency_ms * 2) {
			cd->read_batch = max(cd->read_batch / 2, 2);
			cd->write_batch = max(cd->write_batch / 2, 2);
			cd->write_expire_ms = max(cd->write_expire_ms / 2, 5);
			cd->pressure_thres = max(cd->pressure_thres / 2, 4);
		} else if (avg_latency_ms < cd->target_latency_ms / 2) {
			cd->read_batch = min(cd->read_batch * 2, 64);
			cd->write_batch = min(cd->write_batch * 2, 64);
			cd->write_expire_ms = min(cd->write_expire_ms * 2, 1000);
			cd->pressure_thres = min(cd->pressure_thres * 2, 96);
		}
	} else {
		if (avg_latency_ms > cd->target_latency_ms * 2) {
			cd->read_batch = max(cd->read_batch / 2, 4);
			cd->write_batch = max(cd->write_batch / 2, 4);
			cd->write_expire_ms = max(cd->write_expire_ms / 2, 10);
			cd->pressure_thres = max(cd->pressure_thres / 2, 8);
		} else if (avg_latency_ms < cd->target_latency_ms / 2) {
			cd->read_batch = min(cd->read_batch * 2, 32);
			cd->write_batch = min(cd->write_batch * 2, 32);
			cd->write_expire_ms = min(cd->write_expire_ms * 2, 500);
			cd->pressure_thres = min(cd->pressure_thres * 2, 48);
		}
	}

	read_cnt = cd->queue_counts[PRIO_READ_FG] +
		       cd->queue_counts[PRIO_READ_BG] +
		       cd->queue_counts[PRIO_CRITICAL];
	write_cnt = cd->queue_counts[PRIO_WRITE_FG] +
			cd->queue_counts[PRIO_WRITE_BG];
	total_io = read_cnt + write_cnt;
	if (total_io > 0) {
		read_ratio = read_cnt * 100 / total_io;
		if (read_ratio > 70)
			cd->read_batch = min(cd->read_batch * 2, 64);
		else if (read_ratio < 30)
			cd->write_batch = min(cd->write_batch * 2, 64);
	}

	if (queue_depth > cd->pressure_thres * 2) {
		cd->read_batch = max(cd->read_batch / 2, 4);
		cd->write_batch = max(cd->write_batch / 2, 4);
	}

	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;

	cinnamon_reset_tuning_counters(cd, avg_latency_ms);
	cinnamon_clamp_for_emmc(cd);
}

/* ============================================================================
 * Dispatch function (main scheduler logic)
 * ============================================================================
 */
static int cinnamon_dispatch(struct request_queue *q, int force)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	struct request *rq = NULL;
	struct request *wrq;
	u64 now_ns = ktime_to_ns(ktime_get());
	u64 wait, lat, total_ns;
	int target_pressure = 0;
	int pressure = atomic_read(&cinnamon_io_pressure);
	unsigned long write_expire_ns = cd->write_expire_ms * 1000000ULL;
	unsigned long read_batch = cd->read_batch;
	int prio, i;
	int total_pending;
	unsigned long cur, max_ra, new_ra;
	int num, avg;

	/* Starvation recovery step */
	if (cd->starvation_recovery_until)
		cinnamon_progressive_recovery_step(cd);

	/* Dynamic expiration */
	if (unlikely(pressure == 3))
		write_expire_ns = 50 * 1000000ULL;
	else if (pressure <= 1)
		write_expire_ns = 250 * 1000000ULL;

	if (cd->queue_counts[PRIO_WRITE_FG] + cd->queue_counts[PRIO_WRITE_BG] > 16)
		write_expire_ns = 150 * 1000000ULL;

	/* ASA extended expiration */
	if (cd->asa_active && cd->small_write_count >= ASA_COALESCE_THRESHOLD) {
		unsigned long asa_expire_ns = ASA_WRITE_EXPIRE_MS * 1000000ULL;
		unsigned long asa_max_ns = ASA_MAX_COALESCE_TIME_MS * 1000000ULL;
		if (time_after(jiffies, cd->asa_last_small_write_jiffies +
			       msecs_to_jiffies(ASA_MAX_COALESCE_TIME_MS)))
			write_expire_ns = min(write_expire_ns, asa_max_ns);
		else
			write_expire_ns = max(write_expire_ns, asa_expire_ns);
	}

	/* Background starvation prevention */
	if (cd->fg_dispatch_count >= FG_STARVE_LIMIT) {
		if (!list_empty(&cd->queues[PRIO_READ_BG])) {
			rq = list_first_entry(&cd->queues[PRIO_READ_BG],
					      struct request, queuelist);
			target_pressure = 1;
			cd->fg_dispatch_count = 0;
			goto dispatch;
		}
		if (!list_empty(&cd->queues[PRIO_WRITE_BG])) {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_BG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->fg_dispatch_count = 0;
			goto dispatch;
		}
		cd->fg_dispatch_count = 0;
	}

	/* 1. Critical */
	if (!list_empty(&cd->queues[PRIO_CRITICAL])) {
		rq = list_first_entry(&cd->queues[PRIO_CRITICAL],
				      struct request, queuelist);
		target_pressure = 1;
		cd->fg_dispatch_count++;
		goto dispatch;
	}

	/* 2. TRIM */
	if (!list_empty(&cd->queues[PRIO_TRIM])) {
		rq = list_first_entry(&cd->queues[PRIO_TRIM],
				      struct request, queuelist);
		target_pressure = 0;
		goto dispatch;
	}

	/* 3. Write starvation (foreground writes) */
	if (!list_empty(&cd->queues[PRIO_WRITE_FG])) {
		wrq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
						       struct request, queuelist);
		wait = now_ns - cinnamon_get_req_time(wrq);
		if (wait > write_expire_ns) {
			if (wait > WRITE_STARVE_THRESH_NS) {
				cd->write_starved_count++;
				cd->last_starved_jiffies = jiffies;
				cinnamon_initiate_starvation_recovery(cd);
			}
			rq = wrq;
			target_pressure = 3;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	/* 4. eMMC sequential boost */
	if (cd->emmc_mode && cd->seq_write_count > 4 &&
	    !list_empty(&cd->queues[PRIO_WRITE_FG])) {
		rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
				      struct request, queuelist);
		target_pressure = 2;
		cd->fg_dispatch_count++;
		goto dispatch;
	}

	/* 5. Flush coalescing buffer if timeout */
	if (cd->asa_buffer_active &&
	    time_after(jiffies, cd->asa_buffer_start_time +
		       msecs_to_jiffies(ASA_COALESCE_FLUSH_MS)))
		cinnamon_flush_coalesce_buffer(q);

	/* 6. Read/write interleaving */
	if (!list_empty(&cd->queues[PRIO_READ_FG])) {
		if (cd->read_batch_count >= read_batch &&
		    !list_empty(&cd->queues[PRIO_WRITE_FG])) {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->read_batch_count = 0;
			cd->fg_dispatch_count++;
			goto dispatch;
		} else {
			rq = list_first_entry(&cd->queues[PRIO_READ_FG],
					      struct request, queuelist);
			target_pressure = 1;
			cd->read_batch_count++;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	if (!list_empty(&cd->queues[PRIO_WRITE_FG])) {
		if (cd->write_batch_count >= cd->write_batch &&
		    !list_empty(&cd->queues[PRIO_READ_FG])) {
			rq = list_first_entry(&cd->queues[PRIO_READ_FG],
					      struct request, queuelist);
			target_pressure = 1;
			cd->write_batch_count = 0;
			cd->fg_dispatch_count++;
			goto dispatch;
		} else {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->write_batch_count++;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	/* 7. Background reads */
	if (!list_empty(&cd->queues[PRIO_READ_BG])) {
		rq = list_first_entry(&cd->queues[PRIO_READ_BG],
				      struct request, queuelist);
		target_pressure = 1;
		cd->read_batch_count++;
		cd->fg_dispatch_count = 0;
		goto dispatch;
	}

	/* 8. Background writes */
	if (!list_empty(&cd->queues[PRIO_WRITE_BG])) {
		rq = list_first_entry(&cd->queues[PRIO_WRITE_BG],
				      struct request, queuelist);
		target_pressure = 2;
		cd->fg_dispatch_count = 0;
		goto dispatch;
	}

	/* No requests */
	cd->empty_dispatch_count++;
	cinnamon_set_pressure(cd, 0);

	if (cd->empty_dispatch_count > 32) {
		cd->asa_active = false;
		cd->small_write_count = 0;
		cd->asa_last_sector = 0;
	}

	if ((cd->empty_dispatch_count % 100) == 0)
		cinnamon_adjust_params(cd);

	/* Idle read‑ahead adjustments */
	if ((cd->empty_dispatch_count % 50) == 0) {
		if (cd->predicted_sector &&
		    !cd->queue_counts[PRIO_READ_FG] &&
		    !cd->queue_counts[PRIO_READ_BG]) {
			cur = cd->q->backing_dev_info.ra_pages;
			max_ra = min(cur * 2, 256UL);
			new_ra = clamp(cur + 16, 16UL, max_ra);
			if (cd->last_read_direction)
				cd->q->backing_dev_info.ra_pages = new_ra;
		} else if (cd->q->backing_dev_info.ra_pages > 64) {
			cd->q->backing_dev_info.ra_pages = 64;
		}
	}
	return 0;

dispatch:
	if (rq) {
		prio = cinnamon_get_req_prio(rq);
		if (prio >= 0 && prio < PRIO_COUNT && cd->queue_counts[prio] > 0)
			cd->queue_counts[prio]--;
		list_del_init(&rq->queuelist);

		/* Latency tracking */
		if ((cd->dispatch_seq % ((rq_data_dir(rq) == READ) ? 8 : 16)) == 0) {
			lat = now_ns - cinnamon_get_req_time(rq);
			cinnamon_update_latency(cd, lat);
		}

		/* History update */
		if ((cd->dispatch_seq++ % CINNAMON_HISTORY_UPDATE_INTERVAL) == 0) {
			if (rq_data_dir(rq) == READ)
				cinnamon_update_read_history(cd, blk_rq_pos(rq));
			else
				cinnamon_update_write_history(cd, blk_rq_pos(rq));
		}

		/* Clear timestamp */
		cinnamon_set_req_time_prio(rq, 0, 0);

		/* Update batch counters */
		if (target_pressure == 1) {
			cd->read_batch_count++;
			cd->write_batch_count = 0;
		} else if (target_pressure == 2) {
			cd->write_batch_count++;
			cd->read_batch_count = 0;
		} else {
			cd->read_batch_count = 0;
			cd->write_batch_count = 0;
		}

		/* ASA counters */
		if (rq_data_dir(rq) == WRITE && cinnamon_is_small_write(rq)) {
			if (cd->small_write_count > 0)
				cd->small_write_count--;
			if (cd->small_write_count < ASA_COALESCE_THRESHOLD / 2)
				cd->asa_active = false;
		}

		cinnamon_update_tuning_counters(cd);

		if (PROACTIVE_TUNING) {
			total_ns = atomic64_read(&cd->total_latency_ns);
			num = atomic_read(&cd->num_requests);
			avg = num ? div64_u64(total_ns / num, 1000000) : 0;
			if (cinnamon_should_tune_now(cd, avg))
				cinnamon_adjust_params(cd);
		}

		/* Update pressure */
		total_pending = 0;
		for (i = 0; i < PRIO_COUNT; i++)
			total_pending += cd->queue_counts[i];
		if (total_pending > cd->pressure_thres)
			target_pressure = 3;

		if (target_pressure != atomic_read(&cinnamon_io_pressure))
			cinnamon_set_pressure(cd, target_pressure);

		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

/* ============================================================================
 * Former / latter request helpers (for elevator core)
 * ============================================================================
 */
static struct request *
cinnamon_former_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = cinnamon_get_req_prio(rq);
	if (prio < 0 || prio >= PRIO_COUNT)
		return NULL;
	if (rq->queuelist.prev == &cd->queues[prio])
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
cinnamon_latter_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = cinnamon_get_req_prio(rq);
	if (prio < 0 || prio >= PRIO_COUNT)
		return NULL;
	if (rq->queuelist.next == &cd->queues[prio])
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

/* ============================================================================
 * Init & exit
 * ============================================================================
 */
static int cinnamon_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq = elevator_alloc(q, e);
	struct cinnamon_data *cd;
	int i;
	dev_t devt;
	
	if (!eq)
		return -ENOMEM;

	cd = kmalloc_node(sizeof(*cd), GFP_NOIO, q->node);
	if (!cd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	for (i = 0; i < PRIO_COUNT; i++) {
		INIT_LIST_HEAD(&cd->queues[i]);
		cd->queue_counts[i] = 0;
	}

	cd->read_batch_count = 0;
	cd->write_batch_count = 0;
	cd->fg_dispatch_count = 0;
	atomic64_set(&cd->total_latency_ns, 0);
	atomic_set(&cd->num_requests, 0);
	cd->last_ra_pressure = -1;
	cd->last_pressure_change_jiffies = 0;
	cd->last_manual_param_change_jiffies = jiffies;
	cd->write_expire_ms = CINNAMON_WRITE_EXPIRE;
	cd->read_batch = CINNAMON_READ_BATCH;
	cd->write_batch = CINNAMON_WRITE_BATCH;
	cd->pressure_thres = CINNAMON_PRESSURE_THRES;
	cd->hysteresis_ms = CINNAMON_HYSTERESIS_MS;

	cd->history_index = cd->history_count = 0;
	cd->last_read_direction = 0;
	cd->predicted_sector = 0;
	cd->write_history_index = cd->write_history_count = 0;
	cd->last_write_direction = 0;
	cd->predicted_write_sector = 0;
	cd->seq_write_count = 0;
	cd->last_write_sector = 0;

	cd->last_adjust_jiffies = jiffies;
	cd->target_latency_ms = CINNAMON_TARGET_LATENCY_MS;
	cd->adjust_interval_ms = CINNAMON_ADJUST_INTERVAL_MS;
	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;

	cd->q = q;
	cd->dispatch_seq = 0;
	cd->empty_dispatch_count = 0;

	cd->write_starved_count = 0;
	cd->last_starved_jiffies = 0;
	cd->starvation_recovery_until = 0;
	cd->saved_read_batch_for_starve = 0;
	cd->starvation_recovery_steps = 0;
	cd->starvation_events_count = 0;

	cd->emmc_mode = false;
	if (q->backing_dev_info.dev) {
		devt = q->backing_dev_info.dev->devt;
		if (MAJOR(devt) == EMMC_MAJOR)
			cd->emmc_mode = true;
	}
	cinnamon_clamp_for_emmc(cd);

	cd->small_write_count = 0;
	cd->asa_last_small_write_jiffies = 0;
	cd->asa_merge_count = 0;
	cd->asa_coalesce_count = 0;
	cd->asa_last_sector = 0;
	cd->asa_active = false;

	cd->last_tuning_jiffies = jiffies;
	cd->dispatch_count_since_tuning = 0;
	cd->last_avg_latency_ms = cd->target_latency_ms;
	cd->urgent_tuning_mode = false;
	cd->periodic_tuning_jiffies = jiffies;

	cd->last_pressure_update_jiffies = jiffies;
	cd->requests_since_pressure_update = 0;
	cd->last_calculated_pressure = 0;

	INIT_LIST_HEAD(&cd->asa_coalesce_buffer);
	cd->asa_buffer_count = 0;
	cd->asa_buffer_sectors = 0;
	cd->asa_buffer_start_time = 0;
	cd->asa_buffer_start_sector = 0;
	cd->asa_buffer_active = false;
	cd->asa_coalesced_requests = 0;
	cd->asa_buffer_flushes = 0;

	eq->elevator_data = cd;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void cinnamon_exit_queue(struct elevator_queue *e)
{
	struct cinnamon_data *cd = e->elevator_data;
	struct request *rq, *tmp;
	int i;
	
	if (cd->asa_buffer_active && !list_empty(&cd->asa_coalesce_buffer)) {
		list_for_each_entry_safe(rq, tmp, &cd->asa_coalesce_buffer, queuelist) {
			list_del_init(&rq->queuelist);
			blk_put_request(rq);
		}
	}
	for (i = 0; i < PRIO_COUNT; i++)
		WARN_ON(!list_empty(&cd->queues[i]));
	kfree(cd);
}

/* ============================================================================
 * Sysfs attributes (read‑only and read‑write)
 * ============================================================================
 */
static ssize_t cinnamon_pressure_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", atomic_read(&cinnamon_io_pressure));
}

static ssize_t cinnamon_avg_latency_ns_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	u64 total = atomic64_read(&cd->total_latency_ns);
	int num = atomic_read(&cd->num_requests);
	if (!num)
		return sprintf(page, "0\n");
	return sprintf(page, "%llu\n", (unsigned long long)div64_u64(total, num));
}

#define CINNAMON_ATTR_RW(name) \
	static ssize_t cinnamon_##name##_show(struct elevator_queue *e, char *page) \
	{ struct cinnamon_data *cd = e->elevator_data; return sprintf(page, "%d\n", cd->name); } \
	static ssize_t cinnamon_##name##_store(struct elevator_queue *e, const char *page, size_t count) \
	{ struct cinnamon_data *cd = e->elevator_data; long val; if (kstrtol(page, 10, &val) || val <= 0) return -EINVAL; cd->name = val; cd->last_manual_param_change_jiffies = jiffies; return count; }

CINNAMON_ATTR_RW(write_expire_ms)
CINNAMON_ATTR_RW(read_batch)
CINNAMON_ATTR_RW(write_batch)
CINNAMON_ATTR_RW(pressure_thres)
CINNAMON_ATTR_RW(hysteresis_ms)
CINNAMON_ATTR_RW(target_latency_ms)
CINNAMON_ATTR_RW(adjust_interval_ms)

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
	return sprintf(page,
		"read_batch_count: %u\nwrite_batch_count: %u\nfg_dispatch: %u\n"
		"critical: %d\ntrim: %d\nread_fg: %d\nwrite_fg: %d\nread_bg: %d\nwrite_bg: %d\n",
		cd->read_batch_count, cd->write_batch_count, cd->fg_dispatch_count,
		cd->queue_counts[PRIO_CRITICAL], cd->queue_counts[PRIO_TRIM],
		cd->queue_counts[PRIO_READ_FG], cd->queue_counts[PRIO_WRITE_FG],
		cd->queue_counts[PRIO_READ_BG], cd->queue_counts[PRIO_WRITE_BG]);
}

static ssize_t cinnamon_emmc_mode_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->emmc_mode ? 1 : 0);
}

static ssize_t cinnamon_emmc_mode_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val))
		return -EINVAL;
	cd->emmc_mode = (val != 0);
	cinnamon_clamp_for_emmc(cd);
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_trim_cnt_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->queue_counts[PRIO_TRIM]);
}

static ssize_t cinnamon_starvation_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->write_starved_count);
}

static ssize_t cinnamon_last_starved_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%lu\n", cd->last_starved_jiffies);
}

static ssize_t cinnamon_asa_active_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->asa_active ? 1 : 0);
}

static ssize_t cinnamon_asa_small_write_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->small_write_count);
}

static ssize_t cinnamon_asa_merge_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_merge_count);
}

static ssize_t cinnamon_asa_coalesce_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_coalesce_count);
}

static ssize_t cinnamon_asa_last_sector_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)cd->asa_last_sector);
}

/* Additional read‑only sysfs entries for debugging */
static ssize_t cinnamon_sector_sorted_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", SECTOR_SORTED_INSERT);
}

static ssize_t cinnamon_adaptive_merge_scan_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", ADAPTIVE_MERGE_SCAN);
}

static ssize_t cinnamon_default_merge_scan_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", DEFAULT_MERGE_SCAN);
}

static ssize_t cinnamon_proactive_tuning_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", PROACTIVE_TUNING);
}

static ssize_t cinnamon_dispatch_tuning_interval_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", DISPATCH_TUNING_INTERVAL);
}

static ssize_t cinnamon_periodic_tuning_interval_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", PERIODIC_TUNING_INTERVAL_MS);
}

static ssize_t cinnamon_urgent_tuning_mode_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->urgent_tuning_mode ? 1 : 0);
}

static ssize_t cinnamon_dispatch_count_since_tuning_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->dispatch_count_since_tuning);
}

static ssize_t cinnamon_realtime_pressure_updates_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", REALTIME_PRESSURE_UPDATES);
}

static ssize_t cinnamon_pressure_update_throttle_ms_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", PRESSURE_UPDATE_THROTTLE_MS);
}

static ssize_t cinnamon_pressure_update_interval_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", PRESSURE_UPDATE_INTERVAL);
}

static ssize_t cinnamon_requests_since_pressure_update_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->requests_since_pressure_update);
}

static ssize_t cinnamon_last_calculated_pressure_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->last_calculated_pressure);
}

static ssize_t cinnamon_history_update_interval_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", CINNAMON_HISTORY_UPDATE_INTERVAL);
}

static ssize_t cinnamon_starvation_recovery_until_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%lu\n", cd->starvation_recovery_until);
}

static ssize_t cinnamon_starvation_events_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->starvation_events_count);
}

static ssize_t cinnamon_starvation_recovery_steps_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->starvation_recovery_steps);
}

static ssize_t cinnamon_asa_coalescing_buffer_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", ASA_COALESCING_BUFFER);
}

static ssize_t cinnamon_asa_buffer_active_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->asa_buffer_active ? 1 : 0);
}

static ssize_t cinnamon_asa_buffer_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_buffer_count);
}

static ssize_t cinnamon_asa_buffer_sectors_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_buffer_sectors);
}

static ssize_t cinnamon_asa_coalesced_requests_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_coalesced_requests);
}

static ssize_t cinnamon_asa_buffer_flushes_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->asa_buffer_flushes);
}

static struct elv_fs_entry cinnamon_attrs[] = {
	__ATTR(pressure, 0444, cinnamon_pressure_show, NULL),
	__ATTR(avg_latency_ns, 0444, cinnamon_avg_latency_ns_show, NULL),
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
	__ATTR(emmc_mode, 0644, cinnamon_emmc_mode_show, cinnamon_emmc_mode_store),
	__ATTR(trim_cnt, 0444, cinnamon_trim_cnt_show, NULL),
	__ATTR(starvation_count, 0444, cinnamon_starvation_count_show, NULL),
	__ATTR(last_starved, 0444, cinnamon_last_starved_show, NULL),
	__ATTR(asa_active, 0444, cinnamon_asa_active_show, NULL),
	__ATTR(asa_small_write_count, 0444, cinnamon_asa_small_write_count_show, NULL),
	__ATTR(asa_merge_count, 0444, cinnamon_asa_merge_count_show, NULL),
	__ATTR(asa_coalesce_count, 0444, cinnamon_asa_coalesce_count_show, NULL),
	__ATTR(asa_last_sector, 0444, cinnamon_asa_last_sector_show, NULL),
	__ATTR(sector_sorted, 0444, cinnamon_sector_sorted_show, NULL),
	__ATTR(adaptive_merge_scan, 0444, cinnamon_adaptive_merge_scan_show, NULL),
	__ATTR(default_merge_scan, 0444, cinnamon_default_merge_scan_show, NULL),
	__ATTR(proactive_tuning, 0444, cinnamon_proactive_tuning_show, NULL),
	__ATTR(dispatch_tuning_interval, 0444, cinnamon_dispatch_tuning_interval_show, NULL),
	__ATTR(periodic_tuning_interval, 0444, cinnamon_periodic_tuning_interval_show, NULL),
	__ATTR(urgent_tuning_mode, 0444, cinnamon_urgent_tuning_mode_show, NULL),
	__ATTR(dispatch_count_since_tuning, 0444, cinnamon_dispatch_count_since_tuning_show, NULL),
	__ATTR(realtime_pressure_updates, 0444, cinnamon_realtime_pressure_updates_show, NULL),
	__ATTR(pressure_update_throttle_ms, 0444, cinnamon_pressure_update_throttle_ms_show, NULL),
	__ATTR(pressure_update_interval, 0444, cinnamon_pressure_update_interval_show, NULL),
	__ATTR(requests_since_pressure_update, 0444, cinnamon_requests_since_pressure_update_show, NULL),
	__ATTR(last_calculated_pressure, 0444, cinnamon_last_calculated_pressure_show, NULL),
	__ATTR(history_update_interval, 0444, cinnamon_history_update_interval_show, NULL),
	__ATTR(starvation_recovery_until, 0444, cinnamon_starvation_recovery_until_show, NULL),
	__ATTR(starvation_events_count, 0444, cinnamon_starvation_events_count_show, NULL),
	__ATTR(starvation_recovery_steps, 0444, cinnamon_starvation_recovery_steps_show, NULL),
	__ATTR(asa_coalescing_buffer, 0444, cinnamon_asa_coalescing_buffer_show, NULL),
	__ATTR(asa_buffer_active, 0444, cinnamon_asa_buffer_active_show, NULL),
	__ATTR(asa_buffer_count, 0444, cinnamon_asa_buffer_count_show, NULL),
	__ATTR(asa_buffer_sectors, 0444, cinnamon_asa_buffer_sectors_show, NULL),
	__ATTR(asa_coalesced_requests, 0444, cinnamon_asa_coalesced_requests_show, NULL),
	__ATTR(asa_buffer_flushes, 0444, cinnamon_asa_buffer_flushes_show, NULL),
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
		.elevator_merge_fn		= cinnamon_merge,
	},
	.elevator_name = "cinnamon",
	.elevator_owner = THIS_MODULE,
	.elevator_attrs = cinnamon_attrs,
};

static int __init cinnamon_init(void)
{
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