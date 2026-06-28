#ifndef __LINUX_SPM_H
#define __LINUX_SPM_H

#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

struct spm_page;
struct spm_mm;
struct spm_owner_entry;

/* SPM page flags (for future use) */
#define SPM_PAGE_MERGED		(1 << 0)
#define SPM_PAGE_PRIVATE	(1 << 1)
#define SPM_PAGE_DIRTY		(1 << 2)
#define SPM_PAGE_LOCKED		(1 << 3)

/* SPM system configuration */
struct spm_config {
	unsigned int scan_period_ms;	/* Scanning period (ms) */
	unsigned int max_merge_age;	/* Max age before merging (not used) */
	unsigned int hash_buckets;	/* Number of hash buckets (not used) */
	bool enable_fork_sharing;	/* Enable sharing after fork (disabled) */
	unsigned int merge_threshold;	/* Similarity threshold (not used) */
	unsigned int max_pages_per_scan;/* Max pages to scan per cycle (0 = auto) */
	unsigned int hot_threshold;	/* Scans before page considered hot */
	unsigned int adaptive_rate;	/* 1 = enable adaptive scan rate */
};

/* SPM page tracking structure */
struct spm_page {
	struct page *page;
	struct hlist_node hash_node;
	struct list_head page_node;
	struct list_head owners;
	unsigned long hash;
	struct kref refcount;
	struct mutex mutex;
	bool is_dirty;
	bool is_private;
	bool is_active;
	struct rcu_head rcu;
	unsigned long age;
	unsigned long last_scan;
	int numa_node;
};

/* Per-page, per-mm owner entry */
struct spm_owner_entry {
	struct spm_page *spm_page;
	struct spm_mm *spm_mm;
	struct list_head page_node;
	struct list_head mm_node;
	struct rcu_head rcu;
};

/* Per-mm structure (cached in mm_struct) */
struct spm_mm {
	struct mm_struct *mm;
	unsigned long owner_id;
	atomic_t page_count;
	struct list_head pages;
	bool can_share;
	unsigned long last_scanned_addr;	/* resume point for round‑robin */
	struct rcu_head rcu;
};

/* Slot for tracking active mm_structs (used by scanner) */
struct spm_mm_slot {
	struct mm_struct *mm;
	struct list_head mm_list;
	struct kref kref;
	struct rcu_head rcu;
};

/* Page flags for SPM (defined elsewhere) */
/* PageSpm/SetPageSpm/ClearPageSpm are defined in page-flags.h via PAGEFLAG_FALSE */

/* SPM API functions */
#ifdef CONFIG_SPM
int spm_enter(struct mm_struct *mm);
void spm_exit(struct mm_struct *mm);
int spm_fork(struct mm_struct *mm, struct mm_struct *oldmm);
int spm_merge_page(struct page *page, struct mm_struct *mm,
		   struct vm_area_struct *vma, unsigned long address);
struct page *spm_handle_write_fault(struct vm_area_struct *vma,
				    unsigned long address, pte_t *ptep);
int spm_start_scanner(void);
void spm_stop_scanner(void);
void spm_update_scan_limit(void);

/* Global statistics */
extern atomic_long_t spm_pages_merged;
extern atomic_long_t spm_pages_saved;
extern atomic_long_t spm_scan_cycles;
extern atomic_long_t spm_owner_id_counter;

/* Global state */
extern struct spm_config spm_cfg;
extern struct task_struct *spm_scanner_thread;
extern unsigned long spm_max_pages_per_cycle;
extern int spm_scan_all_vmas;
#else
static inline int spm_enter(struct mm_struct *mm) { return 0; }
static inline void spm_exit(struct mm_struct *mm) { }
static inline int spm_fork(struct mm_struct *mm, struct mm_struct *oldmm) { return 0; }
static inline int spm_merge_page(struct page *page, struct mm_struct *mm,
				 struct vm_area_struct *vma, unsigned long addr) { return 0; }
static inline struct page *spm_handle_write_fault(struct vm_area_struct *vma,
						  unsigned long addr, pte_t *ptep) { return NULL; }
static inline void spm_update_scan_limit(void) { }
#endif

/* Sysfs interface */
#ifdef CONFIG_SYSFS
int spm_sysfs_init(void);
void spm_sysfs_cleanup(void);
#else
static inline int spm_sysfs_init(void) { return 0; }
static inline void spm_sysfs_cleanup(void) { }
#endif

#endif /* __LINUX_SPM_H */