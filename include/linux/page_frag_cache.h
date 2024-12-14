/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/bits.h>
#include <linux/log2.h>
#include <linux/mmdebug.h>
#include <linux/mm_types_task.h>
#include <linux/types.h>

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
/* Use a full byte here to enable assembler optimization as the shift
 * operation is usually expecting a byte.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(7, 0)
#else
/* Compiler should be able to figure out we don't read things as any value
 * ANDed with 0 is 0.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		0
#endif

#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		(PAGE_FRAG_CACHE_ORDER_MASK + 1)

static inline bool encoded_page_decode_pfmemalloc(unsigned long encoded_page)
{
	return !!(encoded_page & PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

static unsigned long encoded_page_decode_order(unsigned long encoded_page)
{
	return encoded_page & PAGE_FRAG_CACHE_ORDER_MASK;
}

static struct page *encoded_page_decode_page(unsigned long encoded_page)
{
        return virt_to_page((void *)encoded_page);
}

/**
 * page_frag_cache_init() - Init page_frag cache.
 * @nc: page_frag cache from which to init
 *
 * Inline helper to initialize the page_frag cache.
 */
static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

/**
 * page_frag_cache_is_pfmemalloc() - Check for pfmemalloc.
 * @nc: page_frag cache from which to check
 *
 * Check if the current page in page_frag cache is allocated from the pfmemalloc
 * reserves. It has the same calling context expectation as the allocation API.
 *
 * Return:
 * true if the current page in page_frag cache is allocated from the pfmemalloc
 * reserves, otherwise return false.
 */
static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return encoded_page_decode_pfmemalloc(nc->encoded_page);
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_cache_prepare(struct page_frag_cache *nc, unsigned int fragsz,
				gfp_t gfp_mask, unsigned int align_mask);
void *__page_frag_alloc_refill_probe_align(struct page_frag_cache *nc,
					   unsigned int fragsz,
					   struct page_frag *pfrag,
					   unsigned int align_mask);

static inline void *page_frag_cache_prepare(struct page_frag_cache *nc,
					    unsigned int fragsz,
					    struct page_frag *pfrag,
					    gfp_t gfp_mask,
					    unsigned int align_mask)
{
	void *va;

	va = __page_frag_cache_prepare(nc, fragsz, gfp_mask, align_mask);
	if (likely(va)) {
		unsigned int offset = nc->offset;
		unsigned long order;

		order = encoded_page_decode_order(nc->encoded_page);
		pfrag->size = (PAGE_SIZE << order) - offset;
		pfrag->page = virt_to_page(va);
		pfrag->offset = offset;
		va += offset;
	}

	return va;
}

/**
 * __page_frag_alloc_align() - Allocate a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the 'va'
 *
 * Allocate a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Virtual address of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_align(struct page_frag_cache *nc,
					    unsigned int fragsz, gfp_t gfp_mask,
					    unsigned int align_mask)
{
	void *va;

	va = __page_frag_cache_prepare(nc, fragsz, gfp_mask, align_mask);
	if (likely(va)) {
		va += nc->offset;
		nc->offset += fragsz;
		nc->pagecnt_bias--;
	}

	return va;
}

/**
 * page_frag_alloc_align() - Allocate a page fragment with aligning requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before allocating a page fragment from
 * page_frag cache with aligning requirement.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_align(struct page_frag_cache *nc,
					  unsigned int fragsz, gfp_t gfp_mask,
					  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, -align);
}

/**
 * page_frag_alloc() - Allocate a page fragment.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Allocate a page fragment from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc(struct page_frag_cache *nc,
				    unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

/**
 * __page_frag_refill_prepare_align() - Prepare refilling a page_frag with
 * aligning requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the fragment
 *
 * Prepare refilling a page_frag from page_frag cache with aligning requirement.
 *
 * Return:
 * True if prepare refilling succeeds, otherwise return false.
 */
static inline bool __page_frag_refill_prepare_align(struct page_frag_cache *nc,
						    unsigned int fragsz,
						    struct page_frag *pfrag,
						    gfp_t gfp_mask,
						    unsigned int align_mask)
{
	return !!page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask,
					 align_mask);
}

/**
 * page_frag_refill_prepare_align() - Prepare refilling a page_frag with
 * aligning requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before prepare refilling a page_frag from
 * page_frag cache with aligning requirement.
 *
 * Return:
 * True if prepare refilling succeeds, otherwise return false.
 */
static inline bool page_frag_refill_prepare_align(struct page_frag_cache *nc,
						  unsigned int fragsz,
						  struct page_frag *pfrag,
						  gfp_t gfp_mask,
						  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_refill_prepare_align(nc, fragsz, pfrag, gfp_mask,
						-align);
}

/**
 * page_frag_refill_prepare() - Prepare refilling a page_frag.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare refilling a page_frag from page_frag cache.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill_prepare(struct page_frag_cache *nc,
					    unsigned int fragsz,
					    struct page_frag *pfrag,
					    gfp_t gfp_mask)
{
	return __page_frag_refill_prepare_align(nc, fragsz, pfrag, gfp_mask,
						~0u);
}

/**
 * page_frag_refill_commit_noref - Commit a prepare refilling without taking
 * refcount.
 * @nc: page_frag cache from which to commit
 * @pfrag: the page_frag to be committed
 * @used_sz: size of the page fragment has been used
 *
 * Commit the prepare refilling by passing the actual used size, but not taking
 * refcount. Mostly used for fragmemt coalescing case when the current fragment
 * can share the same refcount with previous fragment.
 */
static inline void page_frag_refill_commit_noref(struct page_frag_cache *nc,
						 struct page_frag *pfrag,
						 unsigned int used_sz)
{
	VM_BUG_ON(pfrag->size < used_sz);
	VM_BUG_ON(pfrag->offset != nc->offset);
	VM_BUG_ON(pfrag->page != encoded_page_decode_page(nc->encoded_page));

	nc->offset += used_sz;
}

/**
 * __page_frag_alloc_refill_prepare_align() - Prepare allocating a fragment and
 * refilling a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the fragment.
 *
 * Prepare allocating a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void
*__page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
					unsigned int fragsz,
					struct page_frag *pfrag,
					gfp_t gfp_mask, unsigned int align_mask)
{
	return page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask, align_mask);
}

/**
 * page_frag_alloc_refill_prepare_align() - Prepare allocating a fragment and
 * refilling a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement for the fragment.
 *
 * WARN_ON_ONCE() checking for @align before prepare allocating a fragment and
 * refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void
*page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
				      unsigned int fragsz,
				      struct page_frag *pfrag, gfp_t gfp_mask,
				      unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag,
						      gfp_mask, -align);
}

/**
 * page_frag_alloc_refill_prepare() - Prepare allocating a fragment and
 * refilling a page_frag.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare allocating a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_refill_prepare(struct page_frag_cache *nc,
						   unsigned int fragsz,
						   struct page_frag *pfrag,
						   gfp_t gfp_mask)
{
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag,
						      gfp_mask, ~0u);
}

/**
 * page_frag_alloc_refill_probe() - Probe allocating a fragment and refilling
 * a page_frag.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled
 *
 * Probe allocating a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_refill_probe(struct page_frag_cache *nc,
						 unsigned int fragsz,
						 struct page_frag *pfrag)
{
	return __page_frag_alloc_refill_probe_align(nc, fragsz, pfrag, ~0u);
}

/**
 * page_frag_refill_probe() - Probe refilling a page_frag.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled
 *
 * Probe refilling a page_frag from page_frag cache.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill_probe(struct page_frag_cache *nc,
					  unsigned int fragsz,
					  struct page_frag *pfrag)
{
	return !!page_frag_alloc_refill_probe(nc, fragsz, pfrag);
}

/**
 * page_frag_refill_commit - Commit a prepare refilling.
 * @nc: page_frag cache from which to commit
 * @pfrag: the page_frag to be committed
 * @used_sz: size of the page fragment has been used
 *
 * Commit the actual used size for the refill that was prepared.
 */
static inline void page_frag_refill_commit(struct page_frag_cache *nc,
					   struct page_frag *pfrag,
					   unsigned int used_sz)
{
	VM_BUG_ON(!nc->pagecnt_bias);

	nc->pagecnt_bias--;
	page_frag_refill_commit_noref(nc, pfrag, used_sz);
}

void page_frag_free(void *addr);
void page_frag_alloc_abort_ref(struct page_frag_cache *nc, void *va,
			       unsigned int fragsz);

/**
 * page_frag_alloc_abort - Abort the page fragment allocation.
 * @nc: page_frag cache to which the page fragment is aborted back
 * @va: virtual address of page fragment to be aborted
 * @fragsz: size of the page fragment to be aborted
 *
 * It is expected to be called from the same context as the allocation API.
 * Mostly used for error handling cases to abort the fragment allocation knowing
 * that no one else is taking extra reference to the just aborted fragment, so
 * that the aborted fragment can be reused.
 */
static inline void page_frag_alloc_abort(struct page_frag_cache *nc, void *va,
					 unsigned int fragsz)
{
	page_frag_alloc_abort_ref(nc, va, fragsz);
	nc->offset -= fragsz;
}

#endif
