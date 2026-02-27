#ifndef __LINUX_SPM_H
#define __LINUX_SPM_H

/*
 * SuperPageMerge (SPM) - Enhanced memory merging system
 * 
 * Fast, lightweight, and robust page merging for Android
 * Uses mm_struct-based ownership instead of task-based
 */

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

/* SPM page flags */
#define SPM_PAGE_MERGED		(1 << 0)  /* Page is merged */
#define SPM_PAGE_PRIVATE	(1 << 1)  /* Private copy */
#define SPM_PAGE_DIRTY		(1 << 2)  /* Modified since merge */
#define SPM_PAGE_LOCKED		(1 << 3)  /* Locked for processing */

/* SPM system configuration */
struct spm_config {
	unsigned int scan_period_ms;	/* Scanning period */
	unsigned int max_merge_age;	/* Max age before merging */
	unsigned int hash_buckets;	/* Number of hash buckets */
	bool enable_fork_sharing;	/* Enable sharing after fork */
	unsigned int merge_threshold;	/* Similarity threshold percentage */
};

/* Page flags for SPM - defined in page-flags.h via PAGEFLAG(Spm, spm) */
#ifdef CONFIG_SPM
/* No need to redefine PageSpm, SetPageSpm, ClearPageSpm here */
#else
static inline bool PageSpm(struct page *page) { return false; }
static inline void SetPageSpm(struct page *page) { }
static inline void ClearPageSpm(struct page *page) { }
#endif

/* Forward declarations */
struct spm_page;
struct spm_mm;
struct spm_owner_entry;

/* Per-page, per-mm owner entry linking page and mm */
struct spm_owner_entry {
	struct spm_page *spm_page;	/* The SPM page */
	struct spm_mm *spm_mm;		/* The mm structure */
	struct list_head page_node;	/* Node in spm_page->owners */
	struct list_head mm_node;	/* Node in spm_mm->pages */
	struct rcu_head rcu;		/* RCU callback head */
};

/* Per-mm structure for SPM (cached in mm_struct) */
struct spm_mm {
	struct mm_struct *mm;		/* Owner mm_struct */
	unsigned long owner_id;		/* Unique identifier */
	atomic_t page_count;		/* Number of pages owned */
	struct list_head pages;		/* List of spm_owner_entry pages */
	bool can_share;			/* Can share pages with children */
	struct rcu_head rcu;		/* RCU callback */
};

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

/* Global statistics counters */
extern atomic_long_t spm_pages_merged;
extern atomic_long_t spm_pages_saved;
extern atomic_long_t spm_scan_cycles;
extern atomic_long_t spm_owner_id_counter;

/* Global SPM state */
extern struct spm_config spm_cfg;
extern struct task_struct *spm_scanner_thread;

/* SPM mm slot for tracking active mm_structs */
struct spm_mm_slot {
	struct mm_struct *mm;
	struct list_head mm_list;
};
#else
static inline int spm_enter(struct mm_struct *mm) { return 0; }
static inline void spm_exit(struct mm_struct *mm) { }
static inline int spm_fork(struct mm_struct *mm, struct mm_struct *oldmm) { return 0; }
static inline int spm_merge_page(struct page *page, struct mm_struct *mm, 
                                struct vm_area_struct *vma, unsigned long address) { return 0; }
static inline struct page *spm_handle_write_fault(struct vm_area_struct *vma, 
                                                 unsigned long address, pte_t *ptep) { return NULL; }

/* Static definitions for statistics when SPM is disabled */
static atomic_long_t spm_pages_merged;
static atomic_long_t spm_pages_saved;
static atomic_long_t spm_scan_cycles;
static atomic_long_t spm_owner_id_counter;
#endif

/* Sysfs interface */
#ifdef CONFIG_SYSFS
int spm_sysfs_init(void);
void spm_sysfs_cleanup(void);
#else
static inline int spm_sysfs_init(void) { return 0; }
static inline void spm_sysfs_cleanup(void) { }
#endif

#endif /* _LINUX_SPM_H */