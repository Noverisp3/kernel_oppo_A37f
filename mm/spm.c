/*
 * SuperPageMerge (SPM) - Enhanced memory merging system
 * 
 * Fast, lightweight, and robust page merging for Android
 * Uses mm_struct-based ownership instead of task-based
 * 
 * Copyright (C) 2024 Custom Kernel Mod
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/spm.h>
#include <crypto/cinnamon.h>
#include <linux/crc32.h>
#include <linux/hashtable.h>
#include <linux/freezer.h>
#include <linux/oom.h>
#include <linux/numa.h>
#include <linux/rcupdate.h>
#include <linux/mempolicy.h>
#include <linux/module.h>

#include <asm/tlbflush.h>
#include "internal.h"

#ifdef CONFIG_NUMA
#define NUMA(x)		(x)
#define DO_NUMA(x)	do { (x); } while (0)
#else
#define NUMA(x)		(0)
#define DO_NUMA(x)	do { } while (0)
#endif

#include <linux/module.h>

/*
 * SPM page tracking structure with kref for safe lifecycle management
 */
struct spm_page {
	struct page *page;		/* The merged page */
	struct hlist_node hash_node;	/* Hash table node */
	struct list_head page_node;	/* Node in owner's page_list */
	struct list_head owners;	/* List of mm_struct owners */
	unsigned long hash;		/* Page content hash */
	struct kref refcount;		/* Reference count for lifecycle */
	struct mutex mutex;		/* Protect this structure */
	bool is_dirty;			/* Page has been modified */
	bool is_private;		/* Private copy for specific mm */
	bool is_active;			/* Page is actively used */
	struct rcu_head rcu;		/* RCU callback head */
};

/* SPM Configuration */
struct spm_config spm_cfg = {
	.scan_period_ms = 200,		/* Fast scanning - 200ms */
	.max_merge_age = 5,		/* Merge after 5 scans */
	.hash_buckets = 4096,		/* 4K hash buckets */
	.enable_fork_sharing = false,	/* Disable sharing to avoid deadlock */
	.merge_threshold = 95,		/* 95% similarity threshold */
};

/* Forward declarations */
int spm_sysfs_init(void);
void spm_sysfs_cleanup(void);
static void spm_page_release(struct kref *ref);
static void spm_mm_rcu_free(struct rcu_head *rcu);
static void spm_owner_entry_rcu_free(struct rcu_head *rcu);
void spm_scan_pages(void);    /* ← thêm dòng này */

extern int spm_enabled;

/*
 * RCU callback for spm_owner_entry
 */
static void spm_owner_entry_rcu_free(struct rcu_head *rcu)
{
	struct spm_owner_entry *entry = container_of(rcu, struct spm_owner_entry, rcu);
	kfree(entry);
}

/*
 * RCU callback for spm_mm
 */
static void spm_mm_rcu_free(struct rcu_head *rcu)
{
	struct spm_mm *spm_mm = container_of(rcu, struct spm_mm, rcu);
	kfree(spm_mm);
}

/* Global SPM state */
struct workqueue_struct *spm_wq;
struct task_struct *spm_scanner_thread;
static DECLARE_WAIT_QUEUE_HEAD(spm_wait);

/* Hash table for page lookup with RCU protection */
DEFINE_HASHTABLE(spm_page_hash, 12);	/* 4096 buckets */
static DEFINE_SPINLOCK(spm_hash_lock);

/* List of mm_structs using SPM */
static LIST_HEAD(spm_mm_head);
static DEFINE_SPINLOCK(spm_mm_list_lock);

/* Global counters and statistics */
atomic_long_t spm_pages_merged;
atomic_long_t spm_pages_saved;
atomic_long_t spm_scan_cycles;
atomic_long_t spm_owner_id_counter;

/*
 * Calculate hash for page content
 * Uses Cinnamon fast XOR-Rotate hash for Snapdragon 410 optimization
 */
unsigned long spm_hash_page(struct page *page)
{
	void *addr;
	u64 hash = 0;
	
	if (!page || !PageLocked(page))
		return 0;
	
	addr = kmap_atomic(page);
	if (addr) {
#if IS_ENABLED(CONFIG_CINNAMON)
		hash = cinnamon_checksum(addr, PAGE_SIZE);
#else
		/* Fallback to xxhash when cinnamon is not available */
		hash = crc32_le(0, addr, PAGE_SIZE);
#endif
		kunmap_atomic(addr);
	}
	
	return (unsigned long)hash;
}

/*
 * RCU-safe lookup for SPM page by hash
 * Returns page with increased refcount, no lock held
 */
struct spm_page *spm_lookup_page_rcu(unsigned long hash)
{
	struct spm_page *spm_page;
	
	rcu_read_lock();
	hash_for_each_possible_rcu(spm_page_hash, spm_page, hash_node, hash) {
		if (spm_page->hash == hash && kref_get_unless_zero(&spm_page->refcount)) {
			rcu_read_unlock();
			return spm_page;
		}
	}
	rcu_read_unlock();
	return NULL;
}

/*
 * Find SPM page by page pointer
 */
struct spm_page *spm_lookup_page_by_page(struct page *page)
{
	struct spm_page *spm_page;
	unsigned int bkt;
	
	rcu_read_lock();
	hash_for_each_rcu(spm_page_hash, bkt, spm_page, hash_node) {
		if (spm_page->page == page && kref_get_unless_zero(&spm_page->refcount)) {
			rcu_read_unlock();
			return spm_page;
		}
	}
	rcu_read_unlock();
	return NULL;
}

/*
 * Get or create spm_mm for mm_struct with caching
 * Uses cmpxchg for race-free assignment
 */
static struct spm_mm *spm_get_mm(struct mm_struct *mm)
{
	struct spm_mm *spm_mm, *existing_spm_mm;
	
	if (!mm)
		return NULL;
	
	/* Check if spm_mm already exists in mm_struct */
	existing_spm_mm = mm->spm_owner;
	if (existing_spm_mm != NULL)
		return existing_spm_mm;
	
	/* Create new spm_mm */
	spm_mm = kzalloc(sizeof(*spm_mm), GFP_KERNEL);
	if (!spm_mm)
		return NULL;
	
	spm_mm->mm = mm;
	spm_mm->owner_id = atomic_long_inc_return(&spm_owner_id_counter);
	spm_mm->can_share = true;
	INIT_LIST_HEAD(&spm_mm->pages);
	atomic_set(&spm_mm->page_count, 0);
	
	/* Atomically assign spm_mm to mm_struct, handle race */
	existing_spm_mm = cmpxchg(&mm->spm_owner, NULL, spm_mm);
	if (existing_spm_mm != NULL) {
		/* Another thread already assigned spm_mm, free ours */
		kfree(spm_mm);
		return existing_spm_mm;
	}
	
	return spm_mm;
}

/*
 * Add mm_struct as owner of SPM page
 * Creates spm_owner_entry linking page and mm
 */
int spm_add_page_owner(struct spm_page *spm_page, struct mm_struct *mm)
{
	struct spm_mm *spm_mm;
	struct spm_owner_entry *entry;
	
	if (!spm_page || !mm)
		return -EINVAL;
	
	/* Get or create spm_mm for this mm */
	spm_mm = spm_get_mm(mm);
	if (!spm_mm)
		return -ENOMEM;
	
	/* Check if this mm already owns this page */
	mutex_lock(&spm_page->mutex);
	list_for_each_entry(entry, &spm_page->owners, page_node) {
		if (entry->spm_mm->mm == mm) {
			mutex_unlock(&spm_page->mutex);
			return 0;  /* Already owner */
		}
	}
	
	/* Create new owner entry */
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&spm_page->mutex);
		return -ENOMEM;
	}
	
	/* Initialize entry */
	entry->spm_page = spm_page;
	entry->spm_mm = spm_mm;
	INIT_LIST_HEAD(&entry->page_node);
	INIT_LIST_HEAD(&entry->mm_node);
	
	/* Add to both lists */
	list_add_tail(&entry->page_node, &spm_page->owners);
	list_add_tail(&entry->mm_node, &spm_mm->pages);
	
	/* Update counters */
	kref_get(&spm_page->refcount);
	atomic_inc(&spm_mm->page_count);
	spm_page->is_active = true;
	
	mutex_unlock(&spm_page->mutex);
	
	return 0;
}

/*
 * Remove mm_struct as owner of SPM page
 * Frees spm_owner_entry and updates counters
 */
void spm_remove_page_owner(struct spm_page *spm_page, struct mm_struct *mm)
{
	struct spm_owner_entry *entry, *tmp;
	
	if (!spm_page || !mm)
		return;
	
	mutex_lock(&spm_page->mutex);
	list_for_each_entry_safe(entry, tmp, &spm_page->owners, page_node) {
		if (entry->spm_mm->mm == mm) {
			/* Remove from both lists */
			list_del(&entry->page_node);
			list_del(&entry->mm_node);
			
			/* Update counters */
			atomic_dec(&entry->spm_mm->page_count);
			
			/* Free spm_mm if no pages */
			if (atomic_read(&entry->spm_mm->page_count) == 0) {
				/* Clear mm_struct cache using the correct mm */
				entry->spm_mm->mm->spm_owner = NULL;
				call_rcu(&entry->spm_mm->rcu, spm_mm_rcu_free);
			}
			
			/* Release page refcount */
			kref_put(&spm_page->refcount, spm_page_release);
			
			/* Free owner entry */
			call_rcu(&entry->rcu, spm_owner_entry_rcu_free);
			break;
		}
	}
	mutex_unlock(&spm_page->mutex);
}

/*
 * Create new SPM page with proper initialization
 */
static struct spm_page *spm_create_page(struct page *page, unsigned long hash)
{
	struct spm_page *spm_page;
	
	spm_page = kzalloc(sizeof(*spm_page), GFP_KERNEL);
	if (!spm_page)
		return NULL;
	
	spm_page->page = page;
	spm_page->hash = hash;
	kref_init(&spm_page->refcount);
	mutex_init(&spm_page->mutex);
	spm_page->is_dirty = false;
	spm_page->is_private = false;
	spm_page->is_active = true;
	INIT_LIST_HEAD(&spm_page->owners);
	INIT_LIST_HEAD(&spm_page->page_node);
	
	/* Add to hash table */
	spin_lock(&spm_hash_lock);
	hash_add_rcu(spm_page_hash, &spm_page->hash_node, hash);
	spin_unlock(&spm_hash_lock);
	
	SetPageSpm(page);
	
	return spm_page;
}

/*
 * Kref release callback for SPM page
 */
static void spm_page_release(struct kref *ref)
{
	struct spm_page *spm_page = container_of(ref, struct spm_page, refcount);
	
	/* Clear SPM flag from page */
	if (spm_page->page) {
		ClearPageSpm(spm_page->page);
	}
	
	/* Remove from hash table */
	spin_lock(&spm_hash_lock);
	hash_del_rcu(&spm_page->hash_node);
	spin_unlock(&spm_hash_lock);
	
	kfree_rcu(spm_page, rcu);
}

/*
 * Helper function to safely get PMD for address
 * Returns NULL if any level is missing or invalid
 */
static pmd_t *spm_get_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	
	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
		pr_info("SPM: spm_get_pmd - pgd_none or pgd_bad for addr 0x%lx\n", address);
		return NULL;
	}
	
	pud = pud_offset(pgd, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
		pr_info("SPM: spm_get_pmd - pud_none or pud_bad for addr 0x%lx\n", address);
		return NULL;
	}
	
	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd))) {
		pr_info("SPM: spm_get_pmd - pmd_none or pmd_bad for addr 0x%lx\n", address);
		return NULL;
	}
	
	return pmd;
}

/*
 * Merge a page into SPM system with complete page table manipulation
 */
int spm_merge_page(struct page *page, struct mm_struct *mm, 
                   struct vm_area_struct *vma, unsigned long address)
{
	struct spm_page *candidate_page, *new_spm_page;
	unsigned long hash;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	pte_t entry;
	int ret = 0;
	
	pr_info("SPM: spm_merge_page called - page=%p, mm=%p, addr=0x%lx\n", 
         page, mm, address);
	
	if (!page || !mm || !vma || PageSpm(page) || !PageAnon(page)) {
		pr_info("SPM: spm_merge_page - validation failed: page=%p, mm=%p, vma=%p, PageSpm=%d, PageAnon=%d\n",
		         page, mm, vma, PageSpm(page), PageAnon(page));
		return -EINVAL;
	}
	
	hash = spm_hash_page(page);
	if (!hash)
		return -EIO;
	
	/* Look for existing candidate */
	candidate_page = spm_lookup_page_rcu(hash);
	if (candidate_page) {
		/* Add ownership BEFORE locking PTL to avoid sleep while atomic */
		ret = spm_add_page_owner(candidate_page, mm);
		if (ret == 0) {
			/* Get PMD safely */
			pmd = spm_get_pmd(mm, address);
			if (!pmd) {
				/* Rollback: remove owner we just added */
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}
			
			pte = pte_offset_map_lock(mm, pmd, address, &ptl);
			
			if (!pte || !pte_present(*pte)) {
				pte_unmap_unlock(pte, ptl);
				/* Rollback: remove owner we just added */
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}
			
			/* Verify this is still our page (no race) */
			if (pte_page(*pte) != page) {
				pte_unmap_unlock(pte, ptl);
				/* Rollback: remove owner we just added */
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}
			
			/* Atomically replace PTE to point to merged page */
			ptep_clear_flush(vma, address, pte);
			entry = mk_pte(candidate_page->page, vma->vm_page_prot);
			/* Keep read-only for shared pages */
			entry = pte_wrprotect(entry);
			set_pte_at(mm, address, pte, entry);
			
			/* Update rmap */
			page_remove_rmap(page);
			page_add_anon_rmap(candidate_page->page, vma, address);
			
			atomic_long_inc(&spm_pages_saved);
			pte_unmap_unlock(pte, ptl);
		}
		
		kref_put(&candidate_page->refcount, spm_page_release);
	} else {
		/* Create new SPM page */
		new_spm_page = spm_create_page(page, hash);
		if (new_spm_page) {
			ret = spm_add_page_owner(new_spm_page, mm);
			if (ret == 0) {
				atomic_long_inc(&spm_pages_merged);
			}
			kref_put(&new_spm_page->refcount, spm_page_release);
		}
	}
	
	return ret;
}

/*
 * Handle write fault on SPM page with proper NUMA awareness
 */
struct page *spm_handle_write_fault(struct vm_area_struct *vma, 
                                   unsigned long address, pte_t *ptep)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page *old_page, *new_page;
	struct spm_page *spm_page;
	unsigned long hash;
	pmd_t *pmd;
	spinlock_t *ptl;
	pte_t *pte;
	pte_t entry;
	
	old_page = pte_page(*ptep);
	if (!PageSpm(old_page))
		return NULL;
	
	hash = spm_hash_page(old_page);
	if (!hash)
		return NULL;
	
	/* Find SPM page for this hash */
	spm_page = spm_lookup_page_rcu(hash);
	if (!spm_page) {
		pr_warn("SPM: Write fault on unknown SPM page\n");
		return NULL;
	}
	
	/* Get PMD safely */
	pmd = spm_get_pmd(mm, address);
	if (!pmd) {
		kref_put(&spm_page->refcount, spm_page_release);
		return NULL;
	}
	
	/* Allocate new page with NUMA awareness */
	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (!new_page) {
		kref_put(&spm_page->refcount, spm_page_release);
		return NULL;
	}
	
	/* Copy content from original page */
	copy_highpage(new_page, spm_page->page);
	
	/* Increase refcount before removing owner to prevent use-after-free */
	kref_get(&spm_page->refcount);
	
	/* Remove this mm as owner */
	spm_remove_page_owner(spm_page, mm);
	
	/* Update page table with new private page */
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!pte) {
		__free_page(new_page);
		kref_put(&spm_page->refcount, spm_page_release);  /* Return the refcount we added */
		return NULL;
	}
	
	entry = mk_pte(new_page, vma->vm_page_prot);
	entry = pte_mkdirty(entry);
	entry = pte_mkwrite(entry);
	
	/* Atomically replace PTE */
	ptep_clear_flush(vma, address, pte);
	set_pte_at(mm, address, pte, entry);
	
	pte_unmap_unlock(pte, ptl);
	
	/* Update rmap for new page */
	page_add_new_anon_rmap(new_page, vma, address);
	
	/* Mark new page as dirty */
	SetPageDirty(new_page);
	
	/* Release the extra refcount we added */
	kref_put(&spm_page->refcount, spm_page_release);
	
	return new_page;
}

/*
 * SPM scanner thread with rate limiting
 */
static int spm_scanner(void *data)
{
	while (!kthread_should_stop()) {
		spm_scan_pages();
		
		wait_event_interruptible_timeout(spm_wait,
			kthread_should_stop(),
			msecs_to_jiffies(spm_cfg.scan_period_ms));
		
		try_to_freeze();
	}
	
	return 0;
}

/*
 * Scan pages for merging opportunities with proper error handling
 */
void spm_scan_pages(void)
{
	struct spm_mm_slot *mm_slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long addr;
	unsigned long pages_scanned = 0;
	unsigned long pages_merged = 0;
	bool limit_reached = false;
	
	pr_debug("SPM: spm_scan_pages started\n");
	atomic_long_inc(&spm_scan_cycles);
	
	spin_lock(&spm_mm_list_lock);
	if (list_empty(&spm_mm_head)) {
		pr_debug("SPM: spm_scan_pages - no active mm_structs\n");
		spin_unlock(&spm_mm_list_lock);
		return;
	}
	
	list_for_each_entry(mm_slot, &spm_mm_head, mm_list) {
		/* Thử tăng tham chiếu an toàn, nếu thất bại thì mm đang dying */
		if (!atomic_inc_not_zero(&mm_slot->mm->mm_users))
			continue;
		
		mm = mm_slot->mm;
		
		if (!down_read_trylock(&mm->mmap_sem)) {
			mmput(mm);
			continue;
		}
		
		/* Scan all VMAs */
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			if (!(vma->vm_flags & VM_MERGEABLE))
				continue;
			if (vma->vm_flags & VM_DONTCOPY)
				continue;
			
			for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
				struct page *page;
				pte_t *pte;
				pmd_t *pmd;
				spinlock_t *ptl;
				
				pages_scanned++;
				if (pages_scanned > 1000) {
					limit_reached = true;
					break;
				}
				
				pmd = spm_get_pmd(mm, addr);
				if (!pmd)
					continue;
				
				pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
				if (!pte)
					continue;
				
				if (pte_present(*pte)) {
					page = vm_normal_page(vma, addr, *pte);
					if (page && !PageSpm(page) && PageAnon(page)) {
						pr_info("SPM: scan found candidate page=%p, addr=0x%lx\n", page, addr);
						if (trylock_page(page)) {
							pr_info("SPM: trylock_page succeeded for page=%p\n", page);
							if (spm_merge_page(page, mm, vma, addr) == 0) {
								pages_merged++;
								pr_info("SPM: page merged successfully\n");
							} else {
								pr_info("SPM: spm_merge_page failed\n");
							}
							unlock_page(page);
						} else {
							pr_info("SPM: trylock_page failed for page=%p\n", page);
						}
					} else {
						pr_info("SPM: page not suitable - page=%p, PageSpm=%d, PageAnon=%d\n", 
						         page, page ? PageSpm(page) : -1, page ? PageAnon(page) : -1);
					}
				}
				
				pte_unmap_unlock(pte, ptl);
			}
			if (limit_reached)
				break;
		}
		
		up_read(&mm->mmap_sem);
		mmput(mm);  /* Trả lại tham chiếu đã tăng */
		
		if (limit_reached)
			break;
	}
	spin_unlock(&spm_mm_list_lock);
	
	if (pages_merged > 0)
		pr_debug("SPM: Scanned %lu pages, merged %lu pages\n", 
			 pages_scanned, pages_merged);
}

/*
 * Start SPM scanner
 */
int spm_start_scanner(void)
{
	spm_scanner_thread = kthread_run(spm_scanner, NULL, "spm_scanner");
	if (IS_ERR(spm_scanner_thread))
		return PTR_ERR(spm_scanner_thread);
	
	return 0;
}

/*
 * Stop SPM scanner
 */
void spm_stop_scanner(void)
{
	if (spm_scanner_thread) {
		kthread_stop(spm_scanner_thread);
		spm_scanner_thread = NULL;
	}
}

/*
 * Enter SPM for mm_struct with KSM mutual exclusion
 */
int spm_enter(struct mm_struct *mm)
{
	struct spm_mm_slot *mm_slot;
	pr_info("SPM: spm_enter called for mm %p\n", mm);
	
	if (!mm)
		return -EINVAL;
	
	/* Check if already enabled */
	if (test_bit(MMF_VM_MERGEABLE, &mm->flags))
		return 0;
	
	/* Check if KSM is active - mutual exclusion */
	if (test_bit(MMF_VM_MERGEABLE, &mm->flags)) {
		pr_warn("SPM: KSM is already active for mm %p\n", mm);
		pr_warn("SPM: Cannot enable both KSM and SPM simultaneously\n");
		pr_warn("SPM: Please disable KSM first: echo 0 > /sys/kernel/ksm/run\n");
		return -EBUSY;
	}
	
	/* Validate mm_struct */
	if (!mm->mmap || !mm->pgd)
		return -EINVAL;
	
	mm_slot = kzalloc(sizeof(*mm_slot), GFP_KERNEL);
	if (!mm_slot)
		return -ENOMEM;
	
	mm_slot->mm = mm;
	
	/* Add mm slot */
	spin_lock(&spm_mm_list_lock);
	list_add(&mm_slot->mm_list, &spm_mm_head);
	spin_unlock(&spm_mm_list_lock);
	
	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	
	pr_debug("SPM: Enabled merging for mm %p\n", mm);
	return 0;
}

/*
 * Exit SPM for mm_struct with proper cleanup
 */
void spm_exit(struct mm_struct *mm)
{
	struct spm_mm_slot *mm_slot, *tmp;
	struct spm_mm *spm_mm;
	struct spm_owner_entry *entry, *entry_tmp;
	
	if (!mm)
		return;
	
	spm_mm = mm->spm_owner;
	if (spm_mm) {
		/* Remove this mm as owner from all its pages */
		list_for_each_entry_safe(entry, entry_tmp, &spm_mm->pages, mm_node) {
			spm_remove_page_owner(entry->spm_page, mm);
		}
		
		/* Clear owner cache */
		mm->spm_owner = NULL;
		
		/* Free spm_mm if all pages removed */
		if (atomic_read(&spm_mm->page_count) == 0) {
			call_rcu(&spm_mm->rcu, spm_mm_rcu_free);
		} else {
			pr_warn("SPM: mm %p still has %d pages after exit, possible leak\n",
				mm, atomic_read(&spm_mm->page_count));
		}
	}
	
	/* Find and remove mm slot */
	spin_lock(&spm_mm_list_lock);
	list_for_each_entry_safe(mm_slot, tmp, &spm_mm_head, mm_list) {
		if (mm_slot->mm == mm) {
			list_del(&mm_slot->mm_list);
			kfree(mm_slot);
			break;
		}
	}
	spin_unlock(&spm_mm_list_lock);
	
	clear_bit(MMF_VM_MERGEABLE, &mm->flags);
	
	pr_debug("SPM: Disabled merging for mm %p\n", mm);
}

/*
 * Handle fork for SPM with proper inheritance
 * Avoids sleeping in RCU read-side critical section
 */
int spm_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct spm_mm_slot *mm_slot;
	struct spm_mm *old_spm_mm, *new_spm_mm;
	
	if (!test_bit(MMF_VM_MERGEABLE, &oldmm->flags))
		return 0;
	
	/* Validate new mm */
	if (!mm || !mm->mmap || !mm->pgd)
		return -EINVAL;
	
	/* Create mm slot for child */
	mm_slot = kzalloc(sizeof(*mm_slot), GFP_KERNEL);
	if (!mm_slot)
		return -ENOMEM;
	
	mm_slot->mm = mm;
	
	/* Get parent's spm_mm */
	old_spm_mm = oldmm->spm_owner;
	
	/* Create spm_mm for child (always a new one, sharing disabled) */
	new_spm_mm = kzalloc(sizeof(*new_spm_mm), GFP_KERNEL);
	if (!new_spm_mm) {
		kfree(mm_slot);
		return -ENOMEM;
	}
	
	new_spm_mm->mm = mm;
	new_spm_mm->owner_id = atomic_long_inc_return(&spm_owner_id_counter);
	new_spm_mm->can_share = true;
	INIT_LIST_HEAD(&new_spm_mm->pages);
	atomic_set(&new_spm_mm->page_count, 0);
	
	/* Cache spm_mm in child */
	mm->spm_owner = new_spm_mm;
	
	/* Add mm slot */
	spin_lock(&spm_mm_list_lock);
	list_add(&mm_slot->mm_list, &spm_mm_head);
	spin_unlock(&spm_mm_list_lock);
	
	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	
	pr_debug("SPM: Fork without sharing for mm %p\n", mm);
	return 0;
}

/*
 * Initialize SPM system with comprehensive validation
 */
static int __init spm_init(void)
{
	int ret;
	
	pr_info("SPM: SuperPageMerge initializing\n");
	pr_info("SPM: Scan period: %d ms, Hash buckets: %d\n",
		spm_cfg.scan_period_ms, spm_cfg.hash_buckets);
	pr_info("SPM: Merge threshold: %d%%, Fork sharing: %s\n",
		spm_cfg.merge_threshold, spm_cfg.enable_fork_sharing ? "enabled" : "disabled");
	
	/* Validate configuration */
	if (spm_cfg.scan_period_ms < 10 || spm_cfg.scan_period_ms > 10000) {
		pr_err("SPM: Invalid scan period %d, using default 100ms\n", spm_cfg.scan_period_ms);
		spm_cfg.scan_period_ms = 100;
	}
	
	if (spm_cfg.merge_threshold < 50 || spm_cfg.merge_threshold > 100) {
		pr_err("SPM: Invalid merge threshold %d, using default 95%%\n", spm_cfg.merge_threshold);
		spm_cfg.merge_threshold = 95;
	}
	
	/* Initialize hash table */
	hash_init(spm_page_hash);
	
	/* Create workqueue */
	spm_wq = create_singlethread_workqueue("spm");
	if (!spm_wq) {
		pr_err("SPM: Failed to create workqueue\n");
		return -ENOMEM;
	}
	
	/* Initialize statistics */
	atomic_long_set(&spm_pages_merged, 0);
	atomic_long_set(&spm_pages_saved, 0);
	atomic_long_set(&spm_scan_cycles, 0);
	atomic_long_set(&spm_owner_id_counter, 0);
	
	/* Start scanner */
	if (spm_enabled) {
		ret = spm_start_scanner();
		if (ret) {
			destroy_workqueue(spm_wq);
			pr_err("SPM: Failed to start scanner: %d\n", ret);
			return ret;
		}
	}
	
	/* Initialize sysfs interface */
	ret = spm_sysfs_init();
	if (ret) {
		pr_warn("SPM: Failed to initialize sysfs: %d\n", ret);
		/* Continue without sysfs - not fatal */
	}
	
	pr_info("SPM: SuperPageMerge initialized successfully\n");
	pr_info("SPM: Using Cinnamon XOR-Rotate hash for Snapdragon 410 optimization\n");
	
	return 0;
}

/*
 * Cleanup SPM system with comprehensive safety checks
 */
static void __exit spm_module_exit(void)
{
	struct spm_page *spm_page;
	struct hlist_node *node;
	int bkt;
	int pages_remaining = 0;
	int active_refs = 0;
	unsigned long saved;
	unsigned long merged;
	unsigned long efficiency;
	
	pr_info("SPM: SuperPageMerge shutdown requested\n");
	
	/* Stop scanner first */
	spm_stop_scanner();
	
	/* Check if any mm_structs are still using SPM */
	if (!list_empty(&spm_mm_head)) {
		pr_warn("SPM: Cannot unload - mm_structs still active\n");
		pr_warn("SPM: Please kill all processes using SPM first\n");
		return;
	}
	
	/* Check if any pages still have active references */
	hash_for_each_safe(spm_page_hash, bkt, node, spm_page, hash_node) {
		if (atomic_read(&spm_page->refcount.refcount) > 0) {
			active_refs++;
			pages_remaining++;
		}
	}
	
	if (active_refs > 0) {
		pr_warn("SPM: Cannot unload - %d pages still have active references\n", active_refs);
		pr_warn("SPM: Force unload may cause system instability\n");
		return;
	}
	
	/* Cleanup sysfs */
	spm_sysfs_cleanup();
	
	/* Cleanup all remaining SPM pages */
	rcu_barrier(); /* Ensure all RCU readers are done */
	
	/* Print final statistics */
	pr_info("SPM: Final statistics:\n");
	pr_info("SPM:  Pages merged: %ld\n", atomic_long_read(&spm_pages_merged));
	pr_info("SPM:  Pages saved: %ld\n", atomic_long_read(&spm_pages_saved));
	pr_info("SPM:  Scan cycles: %ld\n", atomic_long_read(&spm_scan_cycles));
	pr_info("SPM:  Pages remaining: %d\n", pages_remaining);
	saved = atomic_long_read(&spm_pages_saved);
	merged = atomic_long_read(&spm_pages_merged) + 1;
	efficiency = saved * 10000 / merged;
	pr_info("SPM:  Memory efficiency: %lu.%02lu%%\n", 
	        efficiency / 100, efficiency % 100);
	
	pr_info("SPM: SuperPageMerge shutdown complete\n");
}

module_init(spm_init);
module_exit(spm_module_exit);

MODULE_DESCRIPTION("SuperPageMerge (SPM) - Enhanced memory merging");
MODULE_LICENSE("GPL v2");