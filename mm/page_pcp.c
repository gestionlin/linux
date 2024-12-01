#include <linux/cacheinfo.h>
#include <linux/mm.h>
#include <linux/page-isolation.h>
#include <trace/events/kmem.h>
#include "internal.h"
#include "page_pcp.h"

/* prevent >1 _updater_ of zone percpu pageset ->high and ->batch fields */
static DEFINE_MUTEX(pcp_batch_high_lock);
#define MIN_PERCPU_PAGELIST_HIGH_FRACTION (8)

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/*
 * On SMP, spin_trylock is sufficient protection.
 * On PREEMPT_RT, spin_trylock is equivalent on both SMP and UP.
 */
#define pcp_trylock_prepare(flags)	do { } while (0)
#define pcp_trylock_finish(flag)	do { } while (0)
#else

/* UP spin_trylock always succeeds so disable IRQs to prevent re-entrancy. */
#define pcp_trylock_prepare(flags)	local_irq_save(flags)
#define pcp_trylock_finish(flags)	local_irq_restore(flags)
#endif

/*
 * Locking a pcp requires a PCP lookup followed by a spinlock. To avoid
 * a migration causing the wrong PCP to be locked and remote memory being
 * potentially allocated, pin the task to the CPU for the lookup+lock.
 * preempt_disable is used on !RT because it is faster than migrate_disable.
 * migrate_disable is used on RT because otherwise RT spinlock usage is
 * interfered with and a high priority task cannot preempt the allocator.
 */
#ifndef CONFIG_PREEMPT_RT
#define pcpu_task_pin()		preempt_disable()
#define pcpu_task_unpin()	preempt_enable()
#else
#define pcpu_task_pin()		migrate_disable()
#define pcpu_task_unpin()	migrate_enable()
#endif

/*
 * Generic helper to lookup and a per-cpu variable with an embedded spinlock.
 * Return value should be used with equivalent unlock helper.
 */
#define pcpu_spin_lock(type, member, ptr)				\
({									\
	type *_ret;							\
	pcpu_task_pin();						\
	_ret = this_cpu_ptr(ptr);					\
	spin_lock(&_ret->member);					\
	_ret;								\
})

#define pcpu_spin_trylock(type, member, ptr)				\
({									\
	type *_ret;							\
	pcpu_task_pin();						\
	_ret = this_cpu_ptr(ptr);					\
	if (!spin_trylock(&_ret->member)) {				\
		pcpu_task_unpin();					\
		_ret = NULL;						\
	}								\
	_ret;								\
})

#define pcpu_spin_unlock(member, ptr)					\
({									\
	spin_unlock(&ptr->member);					\
	pcpu_task_unpin();						\
})

/* struct per_cpu_pages specific helpers. */
#define pcp_spin_lock(ptr)						\
	pcpu_spin_lock(struct per_cpu_pages, lock, ptr)

#define pcp_spin_trylock(ptr)						\
	pcpu_spin_trylock(struct per_cpu_pages, lock, ptr)

#define pcp_spin_unlock(ptr)						\
	pcpu_spin_unlock(lock, ptr)

/* These effectively disable the pcplists in the boot pageset completely */
#define BOOT_PAGESET_HIGH	0
#define BOOT_PAGESET_BATCH	1
static DEFINE_PER_CPU(struct per_cpu_pages, boot_pageset);
static DEFINE_PER_CPU(struct per_cpu_zonestat, boot_zonestats);

static DEFINE_MUTEX(pcpu_drain_mutex);

static inline unsigned int order_to_pindex(int migratetype, int order)
{
	bool __maybe_unused movable;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (order > PAGE_ALLOC_COSTLY_ORDER) {
		VM_BUG_ON(order != HPAGE_PMD_ORDER);

		movable = migratetype == MIGRATE_MOVABLE;

		return NR_LOWORDER_PCP_LISTS + movable;
	}
#else
	VM_BUG_ON(order > PAGE_ALLOC_COSTLY_ORDER);
#endif

	return (MIGRATE_PCPTYPES * order) + migratetype;
}

static inline int pindex_to_order(unsigned int pindex)
{
	int order = pindex / MIGRATE_PCPTYPES;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (pindex >= NR_LOWORDER_PCP_LISTS)
		order = HPAGE_PMD_ORDER;
#else
	VM_BUG_ON(order > PAGE_ALLOC_COSTLY_ORDER);
#endif

	return order;
}

/*
 * Frees a number of pages from the PCP lists
 * Assumes all pages on list are in same zone.
 * count is the number of pages to free.
 */
static void free_pcppages_bulk(struct zone *zone, int count,
					struct per_cpu_pages *pcp,
					int pindex)
{
	unsigned long flags;
	unsigned int order;
	struct page *page;

	/*
	 * Ensure proper count is passed which otherwise would stuck in the
	 * below while (list_empty(list)) loop.
	 */
	count = min(pcp->count, count);

	/* Ensure requested pindex is drained first. */
	pindex = pindex - 1;

	spin_lock_irqsave(&zone->lock, flags);

	while (count > 0) {
		struct list_head *list;
		int nr_pages;

		/* Remove pages from lists in a round-robin fashion. */
		do {
			if (++pindex > NR_PCP_LISTS - 1)
				pindex = 0;
			list = &pcp->lists[pindex];
		} while (list_empty(list));

		order = pindex_to_order(pindex);
		nr_pages = 1 << order;
		do {
			unsigned long pfn;
			int mt;

			page = list_last_entry(list, struct page, pcp_list);
			pfn = page_to_pfn(page);
			mt = get_pfnblock_migratetype(page, pfn);

			/* must delete to avoid corrupting pcp list */
			list_del(&page->pcp_list);
			count -= nr_pages;
			pcp->count -= nr_pages;

			free_pcp_page(page, pfn, zone, order, mt);
			trace_mm_page_pcpu_drain(page, order, mt);
		} while (count > 0 && !list_empty(list));
	}

	spin_unlock_irqrestore(&zone->lock, flags);
}

static int nr_pcp_alloc(struct per_cpu_pages *pcp, struct zone *zone, int order)
{
	int high, base_batch, batch, max_nr_alloc;
	int high_max, high_min;

	base_batch = READ_ONCE(pcp->batch);
	high_min = READ_ONCE(pcp->high_min);
	high_max = READ_ONCE(pcp->high_max);
	high = pcp->high = clamp(pcp->high, high_min, high_max);

	/* Check for PCP disabled or boot pageset */
	if (unlikely(high < base_batch))
		return 1;

	if (order)
		batch = base_batch;
	else
		batch = (base_batch << pcp->alloc_factor);

	/*
	 * If we had larger pcp->high, we could avoid to allocate from
	 * zone.
	 */
	if (high_min != high_max && !test_bit(ZONE_BELOW_HIGH, &zone->flags))
		high = pcp->high = min(high + batch, high_max);

	if (!order) {
		max_nr_alloc = max(high - pcp->count - base_batch, base_batch);
		/*
		 * Double the number of pages allocated each time there is
		 * subsequent allocation of order-0 pages without any freeing.
		 */
		if (batch <= max_nr_alloc &&
		    pcp->alloc_factor < CONFIG_PCP_BATCH_SCALE_MAX)
			pcp->alloc_factor++;
		batch = min(batch, max_nr_alloc);
	}

	/*
	 * Scale batch relative to order if batch implies free pages
	 * can be stored on the PCP. Batch can be 1 for small zones or
	 * for boot pagesets which should never store free pages as
	 * the pages may belong to arbitrary zones.
	 */
	if (batch > 1)
		batch = max(batch >> order, 2);

	return batch;
}

/*
 * Called from the vmstat counter updater to decay the PCP high.
 * Return whether there are addition works to do.
 */
int decay_pcp_high(struct zone *zone, struct per_cpu_pages *pcp)
{
	int high_min, to_drain, batch;
	int todo = 0;

	high_min = READ_ONCE(pcp->high_min);
	batch = READ_ONCE(pcp->batch);
	/*
	 * Decrease pcp->high periodically to try to free possible
	 * idle PCP pages.  And, avoid to free too many pages to
	 * control latency.  This caps pcp->high decrement too.
	 */
	if (pcp->high > high_min) {
		pcp->high = max3(pcp->count - (batch << CONFIG_PCP_BATCH_SCALE_MAX),
				 pcp->high - (pcp->high >> 3), high_min);
		if (pcp->high > high_min)
			todo++;
	}

	to_drain = pcp->count - pcp->high;
	if (to_drain > 0) {
		spin_lock(&pcp->lock);
		free_pcppages_bulk(zone, to_drain, pcp, 0);
		spin_unlock(&pcp->lock);
		todo++;
	}

	return todo;
}

#ifdef CONFIG_NUMA
/*
 * Called from the vmstat counter updater to drain pagesets of this
 * currently executing processor on remote nodes after they have
 * expired.
 */
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp)
{
	int to_drain, batch;

	batch = READ_ONCE(pcp->batch);
	to_drain = min(pcp->count, batch);
	if (to_drain > 0) {
		spin_lock(&pcp->lock);
		free_pcppages_bulk(zone, to_drain, pcp, 0);
		spin_unlock(&pcp->lock);
	}
}
#endif

/*
 * Drain pcplists of the indicated processor and zone.
 */
static void drain_pages_zone(unsigned int cpu, struct zone *zone)
{
	struct per_cpu_pages *pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
	int count;

	do {
		spin_lock(&pcp->lock);
		count = pcp->count;
		if (count) {
			int to_drain = min(count,
				pcp->batch << CONFIG_PCP_BATCH_SCALE_MAX);

			free_pcppages_bulk(zone, to_drain, pcp, 0);
			count -= to_drain;
		}
		spin_unlock(&pcp->lock);
	} while (count);
}

/*
 * Drain pcplists of all zones on the indicated processor.
 */
static void drain_pages(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		drain_pages_zone(cpu, zone);
	}
}

/*
 * Spill all of this CPU's per-cpu pages back into the buddy allocator.
 */
void drain_local_pages(struct zone *zone)
{
	int cpu = smp_processor_id();

	if (zone)
		drain_pages_zone(cpu, zone);
	else
		drain_pages(cpu);
}

/*
 * The implementation of drain_all_pages(), exposing an extra parameter to
 * drain on all cpus.
 *
 * drain_all_pages() is optimized to only execute on cpus where pcplists are
 * not empty. The check for non-emptiness can however race with a free to
 * pcplist that has not yet increased the pcp->count from 0 to 1. Callers
 * that need the guarantee that every CPU has drained can disable the
 * optimizing racy check.
 */
static void __drain_all_pages(struct zone *zone, bool force_all_cpus)
{
	int cpu;

	/*
	 * Allocate in the BSS so we won't require allocation in
	 * direct reclaim path for CONFIG_CPUMASK_OFFSTACK=y
	 */
	static cpumask_t cpus_with_pcps;

	/*
	 * Do not drain if one is already in progress unless it's specific to
	 * a zone. Such callers are primarily CMA and memory hotplug and need
	 * the drain to be complete when the call returns.
	 */
	if (unlikely(!mutex_trylock(&pcpu_drain_mutex))) {
		if (!zone)
			return;
		mutex_lock(&pcpu_drain_mutex);
	}

	/*
	 * We don't care about racing with CPU hotplug event
	 * as offline notification will cause the notified
	 * cpu to drain that CPU pcps and on_each_cpu_mask
	 * disables preemption as part of its processing
	 */
	for_each_online_cpu(cpu) {
		struct per_cpu_pages *pcp;
		struct zone *z;
		bool has_pcps = false;

		if (force_all_cpus) {
			/*
			 * The pcp.count check is racy, some callers need a
			 * guarantee that no cpu is missed.
			 */
			has_pcps = true;
		} else if (zone) {
			pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
			if (pcp->count)
				has_pcps = true;
		} else {
			for_each_populated_zone(z) {
				pcp = per_cpu_ptr(z->per_cpu_pageset, cpu);
				if (pcp->count) {
					has_pcps = true;
					break;
				}
			}
		}

		if (has_pcps)
			cpumask_set_cpu(cpu, &cpus_with_pcps);
		else
			cpumask_clear_cpu(cpu, &cpus_with_pcps);
	}

	for_each_cpu(cpu, &cpus_with_pcps) {
		if (zone)
			drain_pages_zone(cpu, zone);
		else
			drain_pages(cpu);
	}

	mutex_unlock(&pcpu_drain_mutex);
}

/*
 * Spill all the per-cpu pages from all CPUs back into the buddy allocator.
 *
 * When zone parameter is non-NULL, spill just the single zone's pages.
 */
void drain_all_pages(struct zone *zone)
{
	__drain_all_pages(zone, false);
}

static int nr_pcp_free(struct per_cpu_pages *pcp, int batch, int high, bool free_high)
{
	int min_nr_free, max_nr_free;

	/* Free as much as possible if batch freeing high-order pages. */
	if (unlikely(free_high))
		return min(pcp->count, batch << CONFIG_PCP_BATCH_SCALE_MAX);

	/* Check for PCP disabled or boot pageset */
	if (unlikely(high < batch))
		return 1;

	/* Leave at least pcp->batch pages on the list */
	min_nr_free = batch;
	max_nr_free = high - batch;

	/*
	 * Increase the batch number to the number of the consecutive
	 * freed pages to reduce zone lock contention.
	 */
	batch = clamp_t(int, pcp->free_count, min_nr_free, max_nr_free);

	return batch;
}

static int nr_pcp_high(struct per_cpu_pages *pcp, struct zone *zone,
		       int batch, bool free_high)
{
	int high, high_min, high_max;

	high_min = READ_ONCE(pcp->high_min);
	high_max = READ_ONCE(pcp->high_max);
	high = pcp->high = clamp(pcp->high, high_min, high_max);

	if (unlikely(!high))
		return 0;

	if (unlikely(free_high)) {
		pcp->high = max(high - (batch << CONFIG_PCP_BATCH_SCALE_MAX),
				high_min);
		return 0;
	}

	/*
	 * If reclaim is active, limit the number of pages that can be
	 * stored on pcp lists
	 */
	if (test_bit(ZONE_RECLAIM_ACTIVE, &zone->flags)) {
		int free_count = max_t(int, pcp->free_count, batch);

		pcp->high = max(high - free_count, high_min);
		return min(batch << 2, pcp->high);
	}

	if (high_min == high_max)
		return high;

	if (test_bit(ZONE_BELOW_HIGH, &zone->flags)) {
		int free_count = max_t(int, pcp->free_count, batch);

		pcp->high = max(high - free_count, high_min);
		high = max(pcp->count, high_min);
	} else if (pcp->count >= high) {
		int need_high = pcp->free_count + batch;

		/* pcp->high should be large enough to hold batch freed pages */
		if (pcp->high < need_high)
			pcp->high = clamp(need_high, high_min, high_max);
	}

	return high;
}

static void free_unref_page_commit(struct zone *zone, struct per_cpu_pages *pcp,
				   struct page *page, int migratetype,
				   unsigned int order)
{
	int high, batch;
	int pindex;
	bool free_high = false;

	/*
	 * On freeing, reduce the number of pages that are batch allocated.
	 * See nr_pcp_alloc() where alloc_factor is increased for subsequent
	 * allocations.
	 */
	pcp->alloc_factor >>= 1;
	__count_vm_events(PGFREE, 1 << order);
	pindex = order_to_pindex(migratetype, order);
	list_add(&page->pcp_list, &pcp->lists[pindex]);
	pcp->count += 1 << order;

	batch = READ_ONCE(pcp->batch);
	/*
	 * As high-order pages other than THP's stored on PCP can contribute
	 * to fragmentation, limit the number stored when PCP is heavily
	 * freeing without allocation. The remainder after bulk freeing
	 * stops will be drained from vmstat refresh context.
	 */
	if (order && order <= PAGE_ALLOC_COSTLY_ORDER) {
		free_high = (pcp->free_count >= batch &&
			     (pcp->flags & PCPF_PREV_FREE_HIGH_ORDER) &&
			     (!(pcp->flags & PCPF_FREE_HIGH_BATCH) ||
			      pcp->count >= READ_ONCE(batch)));
		pcp->flags |= PCPF_PREV_FREE_HIGH_ORDER;
	} else if (pcp->flags & PCPF_PREV_FREE_HIGH_ORDER) {
		pcp->flags &= ~PCPF_PREV_FREE_HIGH_ORDER;
	}
	if (pcp->free_count < (batch << CONFIG_PCP_BATCH_SCALE_MAX))
		pcp->free_count += (1 << order);
	high = nr_pcp_high(pcp, zone, batch, free_high);
	if (pcp->count >= high) {
		free_pcppages_bulk(zone, nr_pcp_free(pcp, batch, high, free_high),
				   pcp, pindex);
		if (test_bit(ZONE_BELOW_HIGH, &zone->flags) &&
		    zone_watermark_ok(zone, 0, high_wmark_pages(zone),
				      ZONE_MOVABLE, 0))
			clear_bit(ZONE_BELOW_HIGH, &zone->flags);
	}
}

bool free_unref_pcp_page(struct zone *zone, struct page *page,
			 unsigned long pfn, unsigned int order)
{
	unsigned long __maybe_unused UP_flags;
	struct per_cpu_pages *pcp;
	int migratetype;

	/*
	 * We only track unmovable, reclaimable and movable on pcp lists.
	 * Place ISOLATE pages on the isolated list because they are being
	 * offlined but treat HIGHATOMIC and CMA as movable pages so we can
	 * get those areas back if necessary. Otherwise, we may have to free
	 * excessively into the page allocator
	 */
	migratetype = get_pfnblock_migratetype(page, pfn);
	if (unlikely(migratetype >= MIGRATE_PCPTYPES)) {
		if (unlikely(is_migrate_isolate(migratetype))) {
			return false;
		}
		migratetype = MIGRATE_MOVABLE;
	}

	pcp_trylock_prepare(UP_flags);
	pcp = pcp_spin_trylock(zone->per_cpu_pageset);
	if (pcp) {
		free_unref_page_commit(zone, pcp, page, migratetype, order);
		pcp_spin_unlock(pcp);
		pcp_trylock_finish(UP_flags);
		return true;
	}

	pcp_trylock_finish(UP_flags);
	return false;
}

void free_unref_pcp_folios(struct folio_batch *folios)
{
	unsigned long __maybe_unused UP_flags;
	struct per_cpu_pages *pcp = NULL;
	struct zone *locked_zone = NULL;
	int i, j;

	for (i = 0, j = 0; i < folios->nr; i++) {
		struct folio *folio = folios->folios[i];
		struct zone *zone = folio_zone(folio);
		unsigned long pfn = folio_pfn(folio);
		unsigned int order = (unsigned long)folio->private;
		int migratetype;

		migratetype = get_pfnblock_migratetype(&folio->page, pfn);

		/* Different zone requires a different pcp lock */
		if (zone != locked_zone ||
		    is_migrate_isolate(migratetype)) {
			if (pcp) {
				pcp_spin_unlock(pcp);
				pcp_trylock_finish(UP_flags);
				locked_zone = NULL;
				pcp = NULL;
			}

			/*
			 * Free isolated pages directly to the
			 * allocator, see comment in free_unref_page.
			 */
			if (is_migrate_isolate(migratetype)) {
				if (j != i)
					folios->folios[j] = folio;

				j++;
				continue;
			}

			/*
			 * trylock is necessary as folios may be getting freed
			 * from IRQ or SoftIRQ context after an IO completion.
			 */
			pcp_trylock_prepare(UP_flags);
			pcp = pcp_spin_trylock(zone->per_cpu_pageset);
			if (unlikely(!pcp)) {
				pcp_trylock_finish(UP_flags);

				if (j != i)
					folios->folios[j] = folio;

				j++;
				continue;
			}
			locked_zone = zone;
		}

		folio->private = NULL;
		/*
		 * Non-isolated types over MIGRATE_PCPTYPES get added
		 * to the MIGRATE_MOVABLE pcp list.
		 */
		if (unlikely(migratetype >= MIGRATE_PCPTYPES))
			migratetype = MIGRATE_MOVABLE;

		trace_mm_page_free_batched(&folio->page);
		free_unref_page_commit(zone, pcp, &folio->page, migratetype,
				order);
	}

	if (pcp) {
		pcp_spin_unlock(pcp);
		pcp_trylock_finish(UP_flags);
	}

	folios->nr = j;
}

/* Remove page from the per-cpu list, caller must protect the list */
static inline
struct page *__rmqueue_pcplist(struct zone *zone, unsigned int order,
			int migratetype,
			unsigned int alloc_flags,
			struct per_cpu_pages *pcp,
			struct list_head *list)
{
	struct page *page;

	do {
		if (list_empty(list)) {
			int batch = nr_pcp_alloc(pcp, zone, order);
			int alloced;

			alloced = rmqueue_bulk(zone, order,
					batch, list,
					migratetype, alloc_flags);

			pcp->count += alloced << order;
			if (unlikely(list_empty(list)))
				return NULL;
		}

		page = list_first_entry(list, struct page, pcp_list);
		list_del(&page->pcp_list);
		pcp->count -= 1 << order;
	} while (check_new_pages(page, order));

	return page;
}

/* Lock and remove page from the per-cpu list */
struct page *rmqueue_pcplist(struct zone *preferred_zone, struct zone *zone,
			     unsigned int order, int migratetype,
			     unsigned int alloc_flags)
{
	struct per_cpu_pages *pcp;
	struct list_head *list;
	struct page *page;
	unsigned long __maybe_unused UP_flags;

	/* spin_trylock may fail due to a parallel drain or IRQ reentrancy. */
	pcp_trylock_prepare(UP_flags);
	pcp = pcp_spin_trylock(zone->per_cpu_pageset);
	if (!pcp) {
		pcp_trylock_finish(UP_flags);
		return NULL;
	}

	/*
	 * On allocation, reduce the number of pages that are batch freed.
	 * See nr_pcp_free() where free_factor is increased for subsequent
	 * frees.
	 */
	pcp->free_count >>= 1;
	list = &pcp->lists[order_to_pindex(migratetype, order)];
	page = __rmqueue_pcplist(zone, order, migratetype, alloc_flags, pcp, list);
	pcp_spin_unlock(pcp);
	pcp_trylock_finish(UP_flags);
	return page;
}

int rmqueue_pcplist_bulk(struct zone *zone, int nr_pages, int migratetype,
			 unsigned int alloc_flags, struct list_head *page_list)
{
	unsigned long __maybe_unused UP_flags;
	struct list_head *pcp_list;
	struct per_cpu_pages *pcp;
	int nr_account = 0;
	struct page *page;

	/* spin_trylock may fail due to a parallel drain or IRQ reentrancy. */
	pcp_trylock_prepare(UP_flags);
	pcp = pcp_spin_trylock(zone->per_cpu_pageset);
	if (!pcp)
		goto failed_try_lock;

	/* Attempt the batch allocation */
	pcp_list = &pcp->lists[order_to_pindex(migratetype, 0)];
	while (nr_account < nr_pages) {
		page = __rmqueue_pcplist(zone, 0, migratetype, alloc_flags,
					 pcp, pcp_list);
		if (unlikely(!page))
			goto out;

		list_add(&page->lru, page_list);
		nr_account++;
	}

out:
	pcp_spin_unlock(pcp);
failed_try_lock:
	pcp_trylock_finish(UP_flags);
	return nr_account;
}

static int zone_batchsize(struct zone *zone)
{
#ifdef CONFIG_MMU
	int batch;

	/*
	 * The number of pages to batch allocate is either ~0.1%
	 * of the zone or 1MB, whichever is smaller. The batch
	 * size is striking a balance between allocation latency
	 * and zone lock contention.
	 */
	batch = min(zone_managed_pages(zone) >> 10, SZ_1M / PAGE_SIZE);
	batch /= 4;		/* We effectively *= 4 below */
	if (batch < 1)
		batch = 1;

	/*
	 * Clamp the batch to a 2^n - 1 value. Having a power
	 * of 2 value was found to be more likely to have
	 * suboptimal cache aliasing properties in some cases.
	 *
	 * For example if 2 tasks are alternately allocating
	 * batches of pages, one task can end up with a lot
	 * of pages of one half of the possible page colors
	 * and the other with pages of the other colors.
	 */
	batch = rounddown_pow_of_two(batch + batch/2) - 1;

	return batch;

#else
	/* The deferral and batching of frees should be suppressed under NOMMU
	 * conditions.
	 *
	 * The problem is that NOMMU needs to be able to allocate large chunks
	 * of contiguous memory as there's no hardware page translation to
	 * assemble apparent contiguous memory from discontiguous pages.
	 *
	 * Queueing large contiguous runs of pages for batching, however,
	 * causes the pages to actually be freed in smaller chunks.  As there
	 * can be a significant delay between the individual batches being
	 * recycled, this leads to the once large chunks of space being
	 * fragmented and becoming unavailable for high-order allocations.
	 */
	return 0;
#endif
}

static int percpu_pagelist_high_fraction;
static int zone_highsize(struct zone *zone, int batch, int cpu_online,
			 int high_fraction)
{
#ifdef CONFIG_MMU
	int high;
	int nr_split_cpus;
	unsigned long total_pages;

	if (!high_fraction) {
		/*
		 * By default, the high value of the pcp is based on the zone
		 * low watermark so that if they are full then background
		 * reclaim will not be started prematurely.
		 */
		total_pages = low_wmark_pages(zone);
	} else {
		/*
		 * If percpu_pagelist_high_fraction is configured, the high
		 * value is based on a fraction of the managed pages in the
		 * zone.
		 */
		total_pages = zone_managed_pages(zone) / high_fraction;
	}

	/*
	 * Split the high value across all online CPUs local to the zone. Note
	 * that early in boot that CPUs may not be online yet and that during
	 * CPU hotplug that the cpumask is not yet updated when a CPU is being
	 * onlined. For memory nodes that have no CPUs, split the high value
	 * across all online CPUs to mitigate the risk that reclaim is triggered
	 * prematurely due to pages stored on pcp lists.
	 */
	nr_split_cpus = cpumask_weight(cpumask_of_node(zone_to_nid(zone))) + cpu_online;
	if (!nr_split_cpus)
		nr_split_cpus = num_online_cpus();
	high = total_pages / nr_split_cpus;

	/*
	 * Ensure high is at least batch*4. The multiple is based on the
	 * historical relationship between high and batch.
	 */
	high = max(high, batch << 2);

	return high;
#else
	return 0;
#endif
}

/*
 * pcp->high and pcp->batch values are related and generally batch is lower
 * than high. They are also related to pcp->count such that count is lower
 * than high, and as soon as it reaches high, the pcplist is flushed.
 *
 * However, guaranteeing these relations at all times would require e.g. write
 * barriers here but also careful usage of read barriers at the read side, and
 * thus be prone to error and bad for performance. Thus the update only prevents
 * store tearing. Any new users of pcp->batch, pcp->high_min and pcp->high_max
 * should ensure they can cope with those fields changing asynchronously, and
 * fully trust only the pcp->count field on the local CPU with interrupts
 * disabled.
 *
 * mutex_is_locked(&pcp_batch_high_lock) required when calling this function
 * outside of boot time (or some other assurance that no concurrent updaters
 * exist).
 */
static void pageset_update(struct per_cpu_pages *pcp, unsigned long high_min,
			   unsigned long high_max, unsigned long batch)
{
	WRITE_ONCE(pcp->batch, batch);
	WRITE_ONCE(pcp->high_min, high_min);
	WRITE_ONCE(pcp->high_max, high_max);
}

static void per_cpu_pages_init(struct per_cpu_pages *pcp, struct per_cpu_zonestat *pzstats)
{
	int pindex;

	memset(pcp, 0, sizeof(*pcp));
	memset(pzstats, 0, sizeof(*pzstats));

	spin_lock_init(&pcp->lock);
	for (pindex = 0; pindex < NR_PCP_LISTS; pindex++)
		INIT_LIST_HEAD(&pcp->lists[pindex]);

	/*
	 * Set batch and high values safe for a boot pageset. A true percpu
	 * pageset's initialization will update them subsequently. Here we don't
	 * need to be as careful as pageset_update() as nobody can access the
	 * pageset yet.
	 */
	pcp->high_min = BOOT_PAGESET_HIGH;
	pcp->high_max = BOOT_PAGESET_HIGH;
	pcp->batch = BOOT_PAGESET_BATCH;
	pcp->free_count = 0;
}

static void __zone_set_pageset_high_and_batch(struct zone *zone, unsigned long high_min,
					      unsigned long high_max, unsigned long batch)
{
	struct per_cpu_pages *pcp;
	int cpu;

	for_each_possible_cpu(cpu) {
		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		pageset_update(pcp, high_min, high_max, batch);
	}
}

/*
 * Calculate and set new high and batch values for all per-cpu pagesets of a
 * zone based on the zone's size.
 */
static void zone_set_pageset_high_and_batch(struct zone *zone, int cpu_online)
{
	int new_high_min, new_high_max, new_batch;

	new_batch = max(1, zone_batchsize(zone));
	if (percpu_pagelist_high_fraction) {
		new_high_min = zone_highsize(zone, new_batch, cpu_online,
					     percpu_pagelist_high_fraction);
		/*
		 * PCP high is tuned manually, disable auto-tuning via
		 * setting high_min and high_max to the manual value.
		 */
		new_high_max = new_high_min;
	} else {
		new_high_min = zone_highsize(zone, new_batch, cpu_online, 0);
		new_high_max = zone_highsize(zone, new_batch, cpu_online,
					     MIN_PERCPU_PAGELIST_HIGH_FRACTION);
	}

	if (zone->pageset_high_min == new_high_min &&
	    zone->pageset_high_max == new_high_max &&
	    zone->pageset_batch == new_batch)
		return;

	zone->pageset_high_min = new_high_min;
	zone->pageset_high_max = new_high_max;
	zone->pageset_batch = new_batch;

	__zone_set_pageset_high_and_batch(zone, new_high_min, new_high_max,
					  new_batch);
}

/*
 * Effectively disable pcplists for the zone by setting the high limit to 0
 * and draining all cpus. A concurrent page freeing on another CPU that's about
 * to put the page on pcplist will either finish before the drain and the page
 * will be drained, or observe the new high limit and skip the pcplist.
 *
 * Must be paired with a call to zone_pcp_enable().
 */
void zone_pcp_disable(struct zone *zone)
{
	mutex_lock(&pcp_batch_high_lock);
	__zone_set_pageset_high_and_batch(zone, 0, 0, 1);
	__drain_all_pages(zone, true);
}

void zone_pcp_enable(struct zone *zone)
{
	__zone_set_pageset_high_and_batch(zone, zone->pageset_high_min,
		zone->pageset_high_max, zone->pageset_batch);
	mutex_unlock(&pcp_batch_high_lock);
}

void zone_pcp_reset(struct zone *zone)
{
	int cpu;
	struct per_cpu_zonestat *pzstats;

	if (zone->per_cpu_pageset != &boot_pageset) {
		for_each_online_cpu(cpu) {
			pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
			drain_zonestat(zone, pzstats);
		}
		free_percpu(zone->per_cpu_pageset);
		zone->per_cpu_pageset = &boot_pageset;
		if (zone->per_cpu_zonestats != &boot_zonestats) {
			free_percpu(zone->per_cpu_zonestats);
			zone->per_cpu_zonestats = &boot_zonestats;
		}
	}
}

void __meminit setup_zone_pageset(struct zone *zone)
{
	int cpu;

	/* Size may be 0 on !SMP && !NUMA */
	if (sizeof(struct per_cpu_zonestat) > 0)
		zone->per_cpu_zonestats = alloc_percpu(struct per_cpu_zonestat);

	zone->per_cpu_pageset = alloc_percpu(struct per_cpu_pages);
	for_each_possible_cpu(cpu) {
		struct per_cpu_pages *pcp;
		struct per_cpu_zonestat *pzstats;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		per_cpu_pages_init(pcp, pzstats);
	}

	zone_set_pageset_high_and_batch(zone, 0);
}

/*
 * The zone indicated has a new number of managed_pages; batch sizes and percpu
 * page high values need to be recalculated.
 */
void zone_pcp_update(struct zone *zone, int cpu_online)
{
	mutex_lock(&pcp_batch_high_lock);
	zone_set_pageset_high_and_batch(zone, cpu_online);
	mutex_unlock(&pcp_batch_high_lock);
}

int percpu_pagelist_high_fraction_set(int new)
{
	struct zone *zone;
	int ret = 0;

	mutex_lock(&pcp_batch_high_lock);

	/* Sanity checking to avoid pcp imbalance */
	if (new && new < MIN_PERCPU_PAGELIST_HIGH_FRACTION) {
		ret = -EINVAL;
		goto out;
	}

	/* No change? */
	if (percpu_pagelist_high_fraction == new)
		goto out;

	percpu_pagelist_high_fraction = new;
	for_each_populated_zone(zone)
		zone_set_pageset_high_and_batch(zone, 0);
out:
	mutex_unlock(&pcp_batch_high_lock);
	return ret;
}

static void zone_pcp_update_cacheinfo(struct zone *zone, unsigned int cpu)
{
	struct per_cpu_pages *pcp;
	struct cpu_cacheinfo *cci;

	pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
	cci = get_cpu_cacheinfo(cpu);
	/*
	 * If data cache slice of CPU is large enough, "pcp->batch"
	 * pages can be preserved in PCP before draining PCP for
	 * consecutive high-order pages freeing without allocation.
	 * This can reduce zone lock contention without hurting
	 * cache-hot pages sharing.
	 */
	spin_lock(&pcp->lock);
	if ((cci->per_cpu_data_slice_size >> PAGE_SHIFT) > 3 * pcp->batch)
		pcp->flags |= PCPF_FREE_HIGH_BATCH;
	else
		pcp->flags &= ~PCPF_FREE_HIGH_BATCH;
	spin_unlock(&pcp->lock);
}

void setup_pcp_cacheinfo(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		zone_pcp_update_cacheinfo(zone, cpu);
}

static int page_alloc_cpu_dead(unsigned int cpu)
{
	struct zone *zone;

	lru_add_drain_cpu(cpu);
	mlock_drain_remote(cpu);
	drain_pages(cpu);

	/*
	 * Spill the event counters of the dead processor
	 * into the current processors event counters.
	 * This artificially elevates the count of the current
	 * processor.
	 */
	vm_events_fold_cpu(cpu);

	/*
	 * Zero the differential counters of the dead processor
	 * so that the vm statistics are consistent.
	 *
	 * This is only okay since the processor is dead and cannot
	 * race with what we are doing.
	 */
	cpu_vm_stats_fold(cpu);

	for_each_populated_zone(zone)
		zone_pcp_update(zone, 0);

	return 0;
}

static int page_alloc_cpu_online(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		zone_pcp_update(zone, 1);
	return 0;
}

void __init page_alloc_init_cpuhp(void)
{
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_PAGE_ALLOC,
					"mm/page_alloc:pcp",
					page_alloc_cpu_online,
					page_alloc_cpu_dead);
	WARN_ON(ret < 0);
}

/*
 * Allocate per cpu pagesets and initialize them.
 * Before this call only boot pagesets were available.
 */
void __init setup_per_cpu_pageset(void)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	int __maybe_unused cpu;

	for_each_populated_zone(zone)
		setup_zone_pageset(zone);

#ifdef CONFIG_NUMA
	/*
	 * Unpopulated zones continue using the boot pagesets.
	 * The numa stats for these pagesets need to be reset.
	 * Otherwise, they will end up skewing the stats of
	 * the nodes these zones are associated with.
	 */
	for_each_possible_cpu(cpu) {
		struct per_cpu_zonestat *pzstats = &per_cpu(boot_zonestats, cpu);
		memset(pzstats->vm_numa_event, 0,
		       sizeof(pzstats->vm_numa_event));
	}
#endif

	for_each_online_pgdat(pgdat)
		pgdat->per_cpu_nodestats =
			alloc_percpu(struct per_cpu_nodestat);
}

void setup_per_cpu_boot_pageset(void)
{
	int cpu;

	/*
	 * Initialize the boot_pagesets that are going to be used
	 * for bootstrapping processors. The real pagesets for
	 * each zone will be allocated later when the per cpu
	 * allocator is available.
	 *
	 * boot_pagesets are used also for bootstrapping offline
	 * cpus if the system is already booted because the pagesets
	 * are needed to initialize allocators on a specific cpu too.
	 * F.e. the percpu allocator needs the page allocator which
	 * needs the percpu allocator in order to allocate its pagesets
	 * (a chicken-egg dilemma).
	 */
	for_each_possible_cpu(cpu)
		per_cpu_pages_init(&per_cpu(boot_pageset, cpu), &per_cpu(boot_zonestats, cpu));
}

__meminit void zone_pcp_init(struct zone *zone)
{
	/*
	 * per cpu subsystem is not up at this point. The following code
	 * relies on the ability of the linker to provide the
	 * offset of a (static) per cpu variable into the per cpu area.
	 */
	zone->per_cpu_pageset = &boot_pageset;
	zone->per_cpu_zonestats = &boot_zonestats;
	zone->pageset_high_min = BOOT_PAGESET_HIGH;
	zone->pageset_high_max = BOOT_PAGESET_HIGH;
	zone->pageset_batch = BOOT_PAGESET_BATCH;

	if (populated_zone(zone))
		pr_debug("  %s zone: %lu pages, LIFO batch:%u\n", zone->name,
			 zone->present_pages, zone_batchsize(zone));
}
