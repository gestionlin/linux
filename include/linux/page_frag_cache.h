/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/gfp.h>
#include <linux/page_frag_types.h>

#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(1, 0)
#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		BIT(2)
#define PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT	2

/**
 * page_frag_cache_init() - Init page_frag cache.
 * @nc: page_frag cache from which to init
 *
 * Inline helper to init the page_frag cache.
 */
static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	memset(nc, 0, sizeof(*nc));
}

/**
 * page_frag_cache_is_pfmemalloc() - Check for pfmemalloc.
 * @nc: page_frag cache from which to check
 *
 * Used to check if the current page in page_frag cache is pfmemalloc'ed.
 * It has the same calling context expection as the alloc API.
 *
 * Return:
 * Return true if the current page in page_frag cache is pfmemalloc'ed,
 * otherwise return false.
 */
static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return (unsigned long)nc->va & PAGE_FRAG_CACHE_PFMEMALLOC_BIT;
}

static inline unsigned int page_frag_cache_page_size(void *va)
{
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	unsigned long page_order;

	page_order = (unsigned long)va & PAGE_FRAG_CACHE_ORDER_MASK;
	return PAGE_SIZE << page_order;
#else
	return PAGE_SIZE;
#endif
}

static inline unsigned int __page_frag_cache_page_offset(void *va,
							 unsigned int size)
{
	return page_frag_cache_page_size(va) - size;
}

/**
 * page_frag_cache_page_offset() - Return the current page fragment's offset.
 * @nc: page_frag cache from which to check
 *
 * The API is only used in net/sched/em_meta.c for historical reason, do not use
 * it for new caller unless there is a strong reason.
 *
 * Return:
 * Return the offset of the current page fragment in the page_frag cache.
 */
static inline unsigned int page_frag_cache_page_offset(struct page_frag_cache *nc)
{
	return __page_frag_cache_page_offset(nc->va, nc->size);
}

/**
 * page_frag_alloc_va() - Alloc a page fragment.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Get a page fragment from page_frag cache.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va(struct page_frag_cache *nc,
				       unsigned int fragsz, gfp_t gfp)
{
	unsigned int offset, size;
	void *va;

	size = nc->size;
	if (unlikely(fragsz > size)) {
		/* fragsz is not supposed to be bigger than PAGE_SIZE as we are
		 * allowing order 3 page allocation to fail easily under low
		 * memory condition.
		 */
		if (WARN_ON_ONCE(fragsz > PAGE_SIZE) ||
		    !__page_frag_cache_refill(nc, gfp))
			return NULL;

		size = nc->size;
	}

	va = nc->va;
	nc->size = size - fragsz;
	nc->pagecnt_bias--;
	offset = __page_frag_cache_page_offset(va, size);
	va = (void *)((unsigned long)va & PAGE_MASK);

	return va + offset;
}

/**
 * __page_frag_alloc_va_align() - Alloc a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement for the 'va'
 *
 * Get a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
					       unsigned int fragsz, gfp_t gfp,
					       unsigned int align_mask)
{
	nc->size = nc->size & align_mask;
	return page_frag_alloc_va(nc, fragsz, gfp);
}

/**
 * page_frag_alloc_va_align() - Alloc a page fragment with aligning requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement for 'va'
 *
 * WARN_ON_ONCE() checking for 'align' before allocing a page fragment from
 * page_frag cache with aligning requirement for 'va'.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_align(struct page_frag_cache *nc,
					     unsigned int fragsz,
					     gfp_t gfp, unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align > PAGE_SIZE);
	return __page_frag_alloc_va_align(nc, fragsz, gfp, -align);
}

/**
 * page_frag_alloc_va_prepare() - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @fragsz: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘fragsz’, 'fragsz' is also used
 * to report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_prepare(struct page_frag_cache *nc,
					       unsigned int *fragsz, gfp_t gfp)
{
	unsigned int offset, size;
	void *va;

	size = nc->size;
	if (unlikely(*fragsz > size)) {
		if (WARN_ON_ONCE(*fragsz > PAGE_SIZE) ||
		    !__page_frag_cache_refill(nc, gfp))
			return NULL;

		size = nc->size;
	}

	va = nc->va;
	*fragsz = size;
	offset = __page_frag_cache_page_offset(va, size);
	va = (void *)((unsigned long)va & PAGE_MASK);
	return va + offset;
}

/**
 * page_frag_alloc_va_prepare_align() - Prepare allocing a page fragment with
 * aligning requirement.
 * @nc: page_frag cache from which to prepare
 * @fragsz: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement for 'va'
 *
 * WARN_ON_ONCE() checking for 'align' before preparing an aligned page fragment
 * with minimum size of ‘fragsz’, 'fragsz' is also used to report the maximum
 * size of the page fragment the caller can use.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_prepare_align(struct page_frag_cache *nc,
						     unsigned int *fragsz,
						     gfp_t gfp,
						     unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align > PAGE_SIZE);
	nc->size = nc->size & -align;
	return page_frag_alloc_va_prepare(nc, fragsz, gfp);
}

/**
 * page_frag_alloc_pg_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @fragsz: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘fragsz’, 'fragsz' is also used
 * to report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * Return the page fragment, otherwise return NULL.
 */
static inline struct page *page_frag_alloc_pg_prepare(struct page_frag_cache *nc,
						      unsigned int *offset,
						      unsigned int *fragsz,
						      gfp_t gfp)
{
	struct page *page;
	unsigned int size;
	void *va;

	size = nc->size;
	if (unlikely(*fragsz > size)) {
		if (WARN_ON_ONCE(*fragsz > PAGE_SIZE)) {
			*fragsz = 0;
			return NULL;
		}

		page = __page_frag_cache_refill(nc, gfp);
		size = nc->size;
		va = nc->va;
	} else {
		va = nc->va;
		page = virt_to_page(va);
	}

	*offset = __page_frag_cache_page_offset(va, size);
	*fragsz = size;

	return page;
}

/**
 * page_frag_alloc_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @fragsz: in as the requested size, out as the available size
 * @va: out as the virtual address of the returned page fragment
 * @gfp: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘fragsz’, 'fragsz' is also used
 * to report the maximum size of the page fragment. Return both 'page' and 'va'
 * of the fragment to the caller.
 *
 * Return:
 * Return the page fragment, otherwise return NULL.
 */
static inline struct page *page_frag_alloc_prepare(struct page_frag_cache *nc,
						   unsigned int *offset,
						   unsigned int *fragsz,
						   void **va, gfp_t gfp)
{
	struct page *page;
	unsigned int size;

	size = nc->size;
	if (unlikely(*fragsz > size)) {
		if (WARN_ON_ONCE(*fragsz > PAGE_SIZE)) {
			*fragsz = 0;
			return NULL;
		}

		page = __page_frag_cache_refill(nc, gfp);
		size = nc->size;
		*va = nc->va;
	} else {
		*va = nc->va;
		page = virt_to_page(*va);
	}

	*offset = __page_frag_cache_page_offset(*va, size);
	*fragsz = size;
	*va = (void *)((unsigned long)*va & PAGE_MASK) + *offset;

	return page;
}

/**
 * page_frag_alloc_probe - Probe the avaiable page fragment.
 * @nc: page_frag cache from which to probe
 * @offset: out as the offset of the page fragment
 * fragsz: in as the requested size, out as the available size
 * va: out as the virtual address of the returned page fragment
 *
 * Probe the current available memory to caller without doing cache refilling.
 * If the cache is empty, return NULL.
 *
 * Return:
 * Return the page fragment, otherwise return NULL.
 */
static inline struct page *page_frag_alloc_probe(struct page_frag_cache *nc,
						 unsigned int *offset,
						 unsigned int *fragsz,
						 void **va)
{
	struct page *page;

	*fragsz = nc->size;

	if (unlikely(!*fragsz))
		return NULL;

	*va = nc->va;
	page = virt_to_page(*va);
	*offset = __page_frag_cache_page_offset(*va, *fragsz);
	*va = (void *)((unsigned long)*va & PAGE_MASK);
	*va += *offset;

	return page;
}

/**
 * page_frag_alloc_commit - Commit allocing a page fragment.
 * @nc: page_frag cache from which to commit
 * @fragsz: size of the page fragment has been used
 *
 * Commit the alloc preparing by passing the actual used size.
 */
static inline void page_frag_alloc_commit(struct page_frag_cache *nc,
					  unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->size || !nc->pagecnt_bias);
	nc->pagecnt_bias--;
	nc->size -= fragsz;
}

/**
 * page_frag_alloc_commit_noref - Commit allocing a page fragment without taking
 * page refcount.
 * @nc: page_frag cache from which to commit
 * @fragsz: size of the page fragment has been used
 *
 * Commit the alloc preparing by passing the actual used size, but not taking
 * page refcount. Mostly used for fragmemt coaleasing case when the current
 * fragmemt can share the same refcount with previous fragmemt.
 */
static inline void page_frag_alloc_commit_noref(struct page_frag_cache *nc,
						unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->size);
	nc->size -= fragsz;
}

#endif
