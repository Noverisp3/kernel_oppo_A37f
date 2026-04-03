/*
 * SuperPageMerge (SPM) - Enhanced memory merging system
 *
 * Fast, lightweight, and robust page merging for Android
 * Uses mm_struct-based ownership instead of task-based
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

/* ----------------------------------------------------------------------
 * SPM page tracking structure (definition already in spm.h)
 * ---------------------------------------------------------------------- */

/* SPM Configuration (default values) */
struct spm_config spm_cfg = {
	.scan_period_ms = 200,
	.max_merge_age = 5,
	.hash_buckets = 4096,
	.enable_fork_sharing = false,
	.merge_threshold = 95,
	.max_pages_per_scan = 0,	/* 0 = auto */
	.hot_threshold = 5,		/* 5 scans before considered hot */
	.adaptive_rate = 1,		/* enable adaptive scan rate */
};

/* Forward declarations */
int spm_sysfs_init(void);
void spm_sysfs_cleanup(void);
static void spm_page_release(struct kref *ref);
static void spm_mm_rcu_free(struct rcu_head *rcu);
static void spm_owner_entry_rcu_free(struct rcu_head *rcu);
static void spm_mm_slot_release(struct kref *kref);
void spm_scan_pages(void);

extern int spm_enabled;

/* Global flag: scan all VMAs (ignore VM_MERGEABLE) */
int spm_scan_all_vmas = 1;

/* ----------------------------------------------------------------------
 * RCU callbacks
 * ---------------------------------------------------------------------- */
static void spm_owner_entry_rcu_free(struct rcu_head *rcu)
{
	struct spm_owner_entry *entry = container_of(rcu, struct spm_owner_entry, rcu);
	kfree(entry);
}

static void spm_mm_rcu_free(struct rcu_head *rcu)
{
	struct spm_mm *spm_mm = container_of(rcu, struct spm_mm, rcu);
	kfree(spm_mm);
}

static void spm_mm_slot_release(struct kref *kref)
{
	struct spm_mm_slot *slot = container_of(kref, struct spm_mm_slot, kref);
	/* Release the mm reference held by the slot */
	mmput(slot->mm);
	kfree_rcu(slot, rcu);
}

/* ----------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */
struct workqueue_struct *spm_wq;
struct task_struct *spm_scanner_thread;
static DECLARE_WAIT_QUEUE_HEAD(spm_wait);

/* Hash table for page lookup with RCU protection */
DEFINE_HASHTABLE(spm_page_hash, 12);	/* 4096 buckets */
static DEFINE_SPINLOCK(spm_hash_lock);

/* List of mm_struct slots (active SPM users) */
static LIST_HEAD(spm_mm_head);
static DEFINE_SPINLOCK(spm_mm_list_lock);

/* Global counters */
atomic_long_t spm_pages_merged;
atomic_long_t spm_pages_saved;
atomic_long_t spm_scan_cycles;
atomic_long_t spm_owner_id_counter;

/* Scanner state */
static unsigned long spm_next_mm_index __maybe_unused;	/* round‑robin (unused but kept) */
static unsigned long spm_scanned_this_cycle;
unsigned long spm_max_pages_per_cycle;	/* exported for sysfs */

/* ----------------------------------------------------------------------
 * Helper: update scan limit based on total RAM and sysfs setting
 * ---------------------------------------------------------------------- */
void spm_update_scan_limit(void)
{
	unsigned long total_ram = totalram_pages;

	/* Default: scan 1% of total RAM per cycle, at least 1024 pages */
	spm_max_pages_per_cycle = max(1024UL, total_ram / 100);

	/* Override if sysfs set a non‑zero value */
	if (spm_cfg.max_pages_per_scan)
		spm_max_pages_per_cycle = spm_cfg.max_pages_per_scan;
}

/* ----------------------------------------------------------------------
 * Adaptive scan rate
 * ---------------------------------------------------------------------- */
static void spm_adjust_scan_rate(void)
{
	unsigned long merged = atomic_long_read(&spm_pages_merged);
	int new_period = spm_cfg.scan_period_ms;

	if (!spm_cfg.adaptive_rate)
		return;

	if (merged > 1000) {
		/* many merges → scan faster */
		new_period = max(10, new_period - 10);
	} else if (merged < 100 && spm_scanned_this_cycle > 0) {
		/* few merges and we scanned something → slow down */
		new_period = min(10000, new_period + 50);
	}
	spm_cfg.scan_period_ms = new_period;
}

/* ----------------------------------------------------------------------
 * Hashing and page comparison
 * ---------------------------------------------------------------------- */
unsigned long spm_hash_page(struct page *page)
{
	void *addr;
	u64 hash = 0;

	if (!page)
		return 0;

	addr = kmap_atomic(page);
	if (addr) {
		hash = crc32_le(0, addr, PAGE_SIZE);
		kunmap_atomic(addr);
	}
	return (unsigned long)hash;
}

static bool spm_pages_equal(struct page *a, struct page *b)
{
	void *addr_a, *addr_b;
	bool equal = false;

	if (a == b)
		return true;

	addr_a = kmap_atomic(a);
	addr_b = kmap_atomic(b);
	if (addr_a && addr_b)
		equal = (memcmp(addr_a, addr_b, PAGE_SIZE) == 0);
	kunmap_atomic(addr_b);
	kunmap_atomic(addr_a);
	return equal;
}

/* ----------------------------------------------------------------------
 * RCU-safe lookups
 * ---------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------
 * spm_mm handling (per-mm structure)
 * ---------------------------------------------------------------------- */
static struct spm_mm *spm_get_mm(struct mm_struct *mm)
{
	struct spm_mm *spm_mm, *existing_spm_mm;

	if (!mm)
		return NULL;

	existing_spm_mm = mm->spm_owner;
	if (existing_spm_mm != NULL)
		return existing_spm_mm;

	spm_mm = kzalloc(sizeof(*spm_mm), GFP_KERNEL);
	if (!spm_mm)
		return NULL;

	spm_mm->mm = mm;
	spm_mm->owner_id = atomic_long_inc_return(&spm_owner_id_counter);
	spm_mm->can_share = true;
	INIT_LIST_HEAD(&spm_mm->pages);
	atomic_set(&spm_mm->page_count, 0);

	existing_spm_mm = cmpxchg(&mm->spm_owner, NULL, spm_mm);
	if (existing_spm_mm != NULL) {
		kfree(spm_mm);
		return existing_spm_mm;
	}
	return spm_mm;
}

int spm_add_page_owner(struct spm_page *spm_page, struct mm_struct *mm)
{
	struct spm_mm *spm_mm;
	struct spm_owner_entry *entry;

	if (!spm_page || !mm)
		return -EINVAL;

	spm_mm = spm_get_mm(mm);
	if (!spm_mm)
		return -ENOMEM;

	mutex_lock(&spm_page->mutex);
	list_for_each_entry(entry, &spm_page->owners, page_node) {
		if (entry->spm_mm->mm == mm) {
			mutex_unlock(&spm_page->mutex);
			return 0;	/* already owner */
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&spm_page->mutex);
		return -ENOMEM;
	}

	entry->spm_page = spm_page;
	entry->spm_mm = spm_mm;
	INIT_LIST_HEAD(&entry->page_node);
	INIT_LIST_HEAD(&entry->mm_node);

	list_add_tail(&entry->page_node, &spm_page->owners);
	list_add_tail(&entry->mm_node, &spm_mm->pages);

	kref_get(&spm_page->refcount);
	atomic_inc(&spm_mm->page_count);
	spm_page->is_active = true;

	mutex_unlock(&spm_page->mutex);
	return 0;
}

void spm_remove_page_owner(struct spm_page *spm_page, struct mm_struct *mm)
{
	struct spm_owner_entry *entry, *tmp;

	if (!spm_page || !mm)
		return;

	mutex_lock(&spm_page->mutex);
	list_for_each_entry_safe(entry, tmp, &spm_page->owners, page_node) {
		if (entry->spm_mm->mm == mm) {
			list_del(&entry->page_node);
			list_del(&entry->mm_node);

			atomic_dec(&entry->spm_mm->page_count);
			if (atomic_read(&entry->spm_mm->page_count) == 0) {
				entry->spm_mm->mm->spm_owner = NULL;
				call_rcu(&entry->spm_mm->rcu, spm_mm_rcu_free);
			}

			kref_put(&spm_page->refcount, spm_page_release);
			call_rcu(&entry->rcu, spm_owner_entry_rcu_free);
			break;
		}
	}
	mutex_unlock(&spm_page->mutex);
}

/* ----------------------------------------------------------------------
 * Page creation and release
 * ---------------------------------------------------------------------- */
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
	spm_page->age = 0;
	spm_page->last_scan = 0;
	spm_page->numa_node = page_to_nid(page);

	spin_lock(&spm_hash_lock);
	hash_add_rcu(spm_page_hash, &spm_page->hash_node, hash);
	spin_unlock(&spm_hash_lock);

	SetPageSpm(page);
	return spm_page;
}

static void spm_page_release(struct kref *ref)
{
	struct spm_page *spm_page = container_of(ref, struct spm_page, refcount);

	if (spm_page->page)
		ClearPageSpm(spm_page->page);

	spin_lock(&spm_hash_lock);
	hash_del_rcu(&spm_page->hash_node);
	spin_unlock(&spm_hash_lock);

	kfree_rcu(spm_page, rcu);
}

/* ----------------------------------------------------------------------
 * Page table helpers
 * ---------------------------------------------------------------------- */
static pmd_t *spm_get_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return NULL;

	pud = pud_offset(pgd, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return NULL;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
		return NULL;

	return pmd;
}

/* ----------------------------------------------------------------------
 * Page merging core
 * ---------------------------------------------------------------------- */
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

	if (!page || !mm || !vma || PageSpm(page) || !PageAnon(page))
		return -EINVAL;

	hash = spm_hash_page(page);
	if (!hash)
		return -EIO;

	candidate_page = spm_lookup_page_rcu(hash);
	if (candidate_page) {
		bool content_match = false;

		/* Verify content before merging */
		if (trylock_page(candidate_page->page) && trylock_page(page)) {
			content_match = spm_pages_equal(candidate_page->page, page);
			unlock_page(page);
			unlock_page(candidate_page->page);
		}
		if (!content_match) {
			kref_put(&candidate_page->refcount, spm_page_release);
			return -EAGAIN;
		}

		/* Add this mm as owner */
		ret = spm_add_page_owner(candidate_page, mm);
		if (ret == 0) {
			/* Update tracking fields */
			candidate_page->age = 0;
			candidate_page->last_scan = atomic_long_read(&spm_scan_cycles);

			/* Replace PTE */
			pmd = spm_get_pmd(mm, address);
			if (!pmd) {
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}

			pte = pte_offset_map_lock(mm, pmd, address, &ptl);
			if (!pte || !pte_present(*pte)) {
				if (pte)
					pte_unmap_unlock(pte, ptl);
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}

			if (pte_page(*pte) != page) {
				pte_unmap_unlock(pte, ptl);
				spm_remove_page_owner(candidate_page, mm);
				kref_put(&candidate_page->refcount, spm_page_release);
				return -EINVAL;
			}

			ptep_clear_flush(vma, address, pte);
			entry = mk_pte(candidate_page->page, vma->vm_page_prot);
			entry = pte_wrprotect(entry);
			set_pte_at(mm, address, pte, entry);

			page_remove_rmap(page);
			page_add_anon_rmap(candidate_page->page, vma, address);

			atomic_long_inc(&spm_pages_saved);
			pte_unmap_unlock(pte, ptl);
		}
		kref_put(&candidate_page->refcount, spm_page_release);
	} else {
		new_spm_page = spm_create_page(page, hash);
		if (new_spm_page) {
			ret = spm_add_page_owner(new_spm_page, mm);
			if (ret == 0)
				atomic_long_inc(&spm_pages_merged);
			kref_put(&new_spm_page->refcount, spm_page_release);
		}
	}
	return ret;
}

/* ----------------------------------------------------------------------
 * Write fault handling (break COW on merged page)
 * ---------------------------------------------------------------------- */
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

	spm_page = spm_lookup_page_rcu(hash);
	if (!spm_page) {
		pr_warn_once("SPM: Write fault on unknown SPM page\n");
		return NULL;
	}

	pmd = spm_get_pmd(mm, address);
	if (!pmd) {
		kref_put(&spm_page->refcount, spm_page_release);
		return NULL;
	}

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (!new_page) {
		kref_put(&spm_page->refcount, spm_page_release);
		return NULL;
	}

	copy_highpage(new_page, spm_page->page);

	kref_get(&spm_page->refcount);	/* extra ref for the removal step */
	spm_remove_page_owner(spm_page, mm);

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!pte) {
		__free_page(new_page);
		kref_put(&spm_page->refcount, spm_page_release);
		return NULL;
	}

	entry = mk_pte(new_page, vma->vm_page_prot);
	entry = pte_mkdirty(entry);
	entry = pte_mkwrite(entry);

	ptep_clear_flush(vma, address, pte);
	set_pte_at(mm, address, pte, entry);

	pte_unmap_unlock(pte, ptl);

	page_add_new_anon_rmap(new_page, vma, address);
	SetPageDirty(new_page);

	kref_put(&spm_page->refcount, spm_page_release);
	return new_page;
}

/* ----------------------------------------------------------------------
 * Scanner thread
 * ---------------------------------------------------------------------- */
static int spm_scanner(void *data)
{
	while (!kthread_should_stop()) {
		spm_scan_pages();
		spm_adjust_scan_rate();

		wait_event_interruptible_timeout(spm_wait,
						  kthread_should_stop(),
						  msecs_to_jiffies(spm_cfg.scan_period_ms));
		try_to_freeze();
	}
	return 0;
}

/*
 * Scan pages for merging opportunities.
 * Uses round‑robin with per‑mm resume pointer and a global page limit.
 */
void spm_scan_pages(void)
{
	struct spm_mm_slot *slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long addr;
	unsigned long pages_scanned = 0;
	unsigned long pages_merged = 0;
	struct spm_mm *spm_mm;
	unsigned long start_vma;

	/* Scan page */
	struct page *page;
	pte_t *pte;
	pmd_t *pmd;
	spinlock_t *ptl;

	atomic_long_inc(&spm_scan_cycles);
	spm_scanned_this_cycle = 0;

	spin_lock(&spm_mm_list_lock);
	if (list_empty(&spm_mm_head)) {
		spin_unlock(&spm_mm_list_lock);
		return;
	}

	/* Iterate over slots, but stop when scan limit is reached */
	list_for_each_entry(slot, &spm_mm_head, mm_list) {
		if (!kref_get_unless_zero(&slot->kref))
			continue;	/* slot is being destroyed */

		spin_unlock(&spm_mm_list_lock);

		mm = slot->mm;
		if (!atomic_inc_not_zero(&mm->mm_users)) {
			kref_put(&slot->kref, spm_mm_slot_release);
			spin_lock(&spm_mm_list_lock);
			continue;
		}

		if (!down_read_trylock(&mm->mmap_sem)) {
			mmput(mm);
			kref_put(&slot->kref, spm_mm_slot_release);
			spin_lock(&spm_mm_list_lock);
			continue;
		}

		spm_mm = mm->spm_owner;
		start_vma = spm_mm ? spm_mm->last_scanned_addr : 0;

		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			if (vma->vm_flags & VM_DONTCOPY)
				continue;
			if (!spm_scan_all_vmas && !(vma->vm_flags & VM_MERGEABLE))
				continue;

			if (start_vma && vma->vm_end <= start_vma)
				continue;

			addr = (start_vma && vma->vm_start <= start_vma) ? start_vma : vma->vm_start;
			for (; addr < vma->vm_end; addr += PAGE_SIZE) {
				spm_scanned_this_cycle++;
				if (spm_scanned_this_cycle >= spm_max_pages_per_cycle) {
					if (spm_mm)
						spm_mm->last_scanned_addr = addr;
					goto out_limit;
				}


				pages_scanned++;
				pmd = spm_get_pmd(mm, addr);
				if (!pmd)
					continue;

				pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
				if (!pte)
					continue;

				if (pte_present(*pte)) {
					page = vm_normal_page(vma, addr, *pte);
					if (page && !PageSpm(page) && PageAnon(page)) {
						struct spm_page *sp = spm_lookup_page_by_page(page);
						if (sp) {
							if ((atomic_long_read(&spm_scan_cycles) - sp->last_scan) < spm_cfg.hot_threshold) {
								kref_put(&sp->refcount, spm_page_release);
								pte_unmap_unlock(pte, ptl);
								ptl = NULL;      // ← thêm dòng này
								continue;
							}
							kref_put(&sp->refcount, spm_page_release);
						}
						if (trylock_page(page)) {
							/* Release PTE lock before sleeping */
							pte_unmap_unlock(pte, ptl);
							ptl = NULL;

							if (spm_merge_page(page, mm, vma, addr) == 0)
								pages_merged++;

							unlock_page(page);
							continue;
						} else {
							pte_unmap_unlock(pte, ptl);
							ptl = NULL;          // ← thêm dòng này
							continue;
						}
					}
				}
				/* If we still hold lock (i.e., no continue happened), unlock it */
				if (ptl)
					pte_unmap_unlock(pte, ptl);
			}
			start_vma = 0;	/* finished this VMA */
		}
		/* Finished this mm, reset its resume point */
		if (spm_mm)
			spm_mm->last_scanned_addr = 0;

out_limit:
		up_read(&mm->mmap_sem);
		mmput(mm);
		kref_put(&slot->kref, spm_mm_slot_release);

		if (spm_scanned_this_cycle >= spm_max_pages_per_cycle)
			break;	/* limit reached, exit loop */

		spin_lock(&spm_mm_list_lock);
	}
	spin_unlock(&spm_mm_list_lock);

	if (pages_merged > 0)
		pr_debug("SPM: scanned %lu pages, merged %lu\n", pages_scanned, pages_merged);
}

/* ----------------------------------------------------------------------
 * Scanner start/stop
 * ---------------------------------------------------------------------- */
int spm_start_scanner(void)
{
	spm_scanner_thread = kthread_run(spm_scanner, NULL, "spm_scanner");
	if (IS_ERR(spm_scanner_thread))
		return PTR_ERR(spm_scanner_thread);
	return 0;
}

void spm_stop_scanner(void)
{
	if (spm_scanner_thread) {
		kthread_stop(spm_scanner_thread);
		spm_scanner_thread = NULL;
	}
}

/* ----------------------------------------------------------------------
 * SPM enable/disable for an mm_struct
 * ---------------------------------------------------------------------- */
int spm_enter(struct mm_struct *mm)
{
	struct spm_mm_slot *slot;

	if (!mm)
		return -EINVAL;

	/* Tăng tham chiếu để mm không bị free khi slot còn tồn tại */
	if (!atomic_inc_not_zero(&mm->mm_users))
		return -EAGAIN;   /* mm đang dying */

	if (test_bit(MMF_VM_MERGEABLE, &mm->flags)) {
		mmput(mm);
		return 0;
	}

	/* KSM mutual exclusion (simplified) */
	if (test_bit(MMF_VM_MERGEABLE, &mm->flags)) {
		mmput(mm);
		pr_warn("SPM: KSM already active for mm %p\n", mm);
		return -EBUSY;
	}
	if (!mm->mmap || !mm->pgd) {
		mmput(mm);
		return -EINVAL;
	}

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot) {
		mmput(mm);
		return -ENOMEM;
	}

	slot->mm = mm;
	kref_init(&slot->kref);

	spin_lock(&spm_mm_list_lock);
	list_add(&slot->mm_list, &spm_mm_head);
	spin_unlock(&spm_mm_list_lock);

	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	pr_debug("SPM: enabled for mm %p\n", mm);
	/* Không mmput ở đây, vì slot đang giữ mm */
	return 0;
}

void spm_exit(struct mm_struct *mm)
{
	struct spm_mm_slot *slot, *tmp;
	struct spm_mm *spm_mm;
	struct spm_owner_entry *entry, *entry_tmp;

	if (!mm)
		return;

	/* Remove all page owners for this mm */
	spm_mm = mm->spm_owner;
	if (spm_mm) {
		list_for_each_entry_safe(entry, entry_tmp, &spm_mm->pages, mm_node)
			spm_remove_page_owner(entry->spm_page, mm);
		mm->spm_owner = NULL;
		if (atomic_read(&spm_mm->page_count) == 0)
			call_rcu(&spm_mm->rcu, spm_mm_rcu_free);
		else
			pr_warn("SPM: mm %p still has %d pages after exit\n",
				mm, atomic_read(&spm_mm->page_count));
	}

	/* Remove from mm slot list */
	spin_lock(&spm_mm_list_lock);
	list_for_each_entry_safe(slot, tmp, &spm_mm_head, mm_list) {
		if (slot->mm == mm) {
			list_del(&slot->mm_list);
			kref_put(&slot->kref, spm_mm_slot_release);
			break;
		}
	}
	spin_unlock(&spm_mm_list_lock);

	clear_bit(MMF_VM_MERGEABLE, &mm->flags);
	pr_debug("SPM: disabled for mm %p\n", mm);
}

/* ----------------------------------------------------------------------
 * Fork handling
 * ---------------------------------------------------------------------- */
int spm_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct spm_mm_slot *slot;
	struct spm_mm *new_spm_mm;

	if (!test_bit(MMF_VM_MERGEABLE, &oldmm->flags))
		return 0;
	if (!mm || !mm->mmap || !mm->pgd)
		return -EINVAL;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	slot->mm = mm;
	kref_init(&slot->kref);

	new_spm_mm = kzalloc(sizeof(*new_spm_mm), GFP_KERNEL);
	if (!new_spm_mm) {
		kfree(slot);
		return -ENOMEM;
	}

	new_spm_mm->mm = mm;
	new_spm_mm->owner_id = atomic_long_inc_return(&spm_owner_id_counter);
	new_spm_mm->can_share = true;
	INIT_LIST_HEAD(&new_spm_mm->pages);
	atomic_set(&new_spm_mm->page_count, 0);

	mm->spm_owner = new_spm_mm;

	spin_lock(&spm_mm_list_lock);
	list_add(&slot->mm_list, &spm_mm_head);
	spin_unlock(&spm_mm_list_lock);

	set_bit(MMF_VM_MERGEABLE, &mm->flags);
	pr_debug("SPM: forked mm %p (sharing disabled)\n", mm);
	return 0;
}

/* ----------------------------------------------------------------------
 * Module init/exit
 * ---------------------------------------------------------------------- */
static int __init spm_init(void)
{
	int ret;

	pr_info("SPM: SuperPageMerge initializing\n");
	pr_info("SPM: scan period %d ms, hot threshold %u, adaptive %s\n",
		spm_cfg.scan_period_ms, spm_cfg.hot_threshold,
		spm_cfg.adaptive_rate ? "on" : "off");

	/* Validate config */
	if (spm_cfg.scan_period_ms < 10 || spm_cfg.scan_period_ms > 10000)
		spm_cfg.scan_period_ms = 100;
	if (spm_cfg.merge_threshold < 50 || spm_cfg.merge_threshold > 100)
		spm_cfg.merge_threshold = 95;

	spm_update_scan_limit();

	hash_init(spm_page_hash);

	spm_wq = create_singlethread_workqueue("spm");
	if (!spm_wq) {
		pr_err("SPM: failed to create workqueue\n");
		return -ENOMEM;
	}

	atomic_long_set(&spm_pages_merged, 0);
	atomic_long_set(&spm_pages_saved, 0);
	atomic_long_set(&spm_scan_cycles, 0);
	atomic_long_set(&spm_owner_id_counter, 0);

	if (spm_enabled) {
		ret = spm_start_scanner();
		if (ret) {
			destroy_workqueue(spm_wq);
			pr_err("SPM: failed to start scanner\n");
			return ret;
		}
	}

	ret = spm_sysfs_init();
	if (ret)
		pr_warn("SPM: sysfs init failed (continuing)\n");

	pr_info("SPM: initialized successfully\n");
	return 0;
}

static void __exit spm_module_exit(void)
{
	struct spm_page *spm_page;
	struct hlist_node *node;
	int bkt, pages_remaining = 0, active_refs = 0;
	unsigned long saved, merged, efficiency;

	pr_info("SPM: shutdown requested\n");
	spm_stop_scanner();

	if (!list_empty(&spm_mm_head)) {
		pr_warn("SPM: cannot unload - mm_structs still active\n");
		return;
	}

	hash_for_each_safe(spm_page_hash, bkt, node, spm_page, hash_node) {
		if (atomic_read(&spm_page->refcount.refcount) > 0) {
			active_refs++;
			pages_remaining++;
		}
	}
	if (active_refs > 0) {
		pr_warn("SPM: cannot unload - %d pages still have active refs\n", active_refs);
		return;
	}

	spm_sysfs_cleanup();
	rcu_barrier();

	pr_info("SPM: final statistics\n");
	pr_info("  pages merged: %ld\n", atomic_long_read(&spm_pages_merged));
	pr_info("  pages saved: %ld\n", atomic_long_read(&spm_pages_saved));
	pr_info("  scan cycles: %ld\n", atomic_long_read(&spm_scan_cycles));
	pr_info("  pages remaining: %d\n", pages_remaining);
	saved = atomic_long_read(&spm_pages_saved);
	merged = atomic_long_read(&spm_pages_merged) + 1;
	efficiency = saved * 10000 / merged;
	pr_info("  memory efficiency: %lu.%02lu%%\n", efficiency / 100, efficiency % 100);

	pr_info("SPM: shutdown complete\n");
}

module_init(spm_init);
module_exit(spm_module_exit);

MODULE_DESCRIPTION("SuperPageMerge (SPM) - Enhanced memory merging");
MODULE_LICENSE("GPL v2");