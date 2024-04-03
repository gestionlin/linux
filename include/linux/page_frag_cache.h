/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/gfp.h>

#define PAGE_FRAG_CACHE_MAX_SIZE	__ALIGN_MASK(32768, ~PAGE_MASK)
#define PAGE_FRAG_CACHE_MAX_ORDER	get_order(PAGE_FRAG_CACHE_MAX_SIZE)

struct page_frag_cache {
	union {
		void *va;
		/* we maintain a pagecount bias, so that we dont dirty cache
		 * line containing page->_refcount every time we allocate a
		 * fragment. As 'va' is always aligned with the order of the
		 * page allocated, we can reuse the LSB bits for the pagecount
		 * bias, and its bit width happens to be indicated by the
		 * 'size_mask' below.
		 */
		unsigned long pagecnt_bias;

	};
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	__u16 offset;
	__u16 size_mask:15;
	__u16 pfmemalloc:1;
#else
	__u32 offset:31;
	__u32 pfmemalloc:1;
#endif
};

/**
 * page_frag_cache_init() - Init page_frag cache.
 * @nc: page_frag cache from which to init
 *
 * Inline helper to init the page_frag cache.
 */
static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->va = NULL;
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
	return !!nc->pfmemalloc;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *page_frag_cache_refill(struct page_frag_cache *nc, unsigned int fragsz,
			     gfp_t gfp_mask);

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
				       unsigned int fragsz, gfp_t gfp_mask)
{
	unsigned int offset;
	void *va;

	va = page_frag_cache_refill(nc, fragsz, gfp_mask);
	if (unlikely(!va))
		return NULL;

	offset = nc->offset;
	nc->pagecnt_bias--;
	nc->offset = offset + fragsz;

	return va + offset;
}

/**
 * __page_frag_alloc_va_align() - Alloc a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement
 *
 * Get a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
					       unsigned int fragsz,
					       gfp_t gfp_mask,
					       unsigned int align)
{
	unsigned int offset = nc->offset;

	nc->offset = ALIGN(offset, align);

	return page_frag_alloc_va(nc, fragsz, gfp_mask);
}

/**
 * page_frag_alloc_va_align() - Alloc a page fragment with aligning requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement
 *
 * WARN_ON_ONCE() checking for align and fragsz before getting a page fragment
 * from page_frag cache with aligning requirement.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_align(struct page_frag_cache *nc,
					     unsigned int fragsz,
					     gfp_t gfp_mask,
					     unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align >= PAGE_SIZE ||
		     fragsz < sizeof(unsigned int));

	return __page_frag_alloc_va_align(nc, fragsz, gfp_mask, align);
}

/**
 * page_frag_alloc_va_prepare() - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @size: in as the requested size, out as the available size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘size’, 'size' is also used to
 * report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_prepare(struct page_frag_cache *nc,
					       unsigned int *offset,
					       unsigned int *size,
					       gfp_t gfp_mask)
{
	void *va;

	va = page_frag_cache_refill(nc, *size, gfp_mask);
	if (unlikely(!va))
		return NULL;

	*offset = nc->offset;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	*size = nc->size_mask - *offset + 1;
#else
	*size = PAGE_SIZE - *offset;
#endif

	return va + *offset;
}

/**
 * page_frag_alloc_va_prepare_align() - Prepare allocing a page fragment with
 * aligning requirement.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @size: in as the requested size, out as the available size
 * @align: the requested aligning requirement
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare an aligned page fragment with minimum size of ‘size’, 'size' is also
 * used to report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_va_prepare_align(struct page_frag_cache *nc,
						     unsigned int *offset,
						     unsigned int *size,
						     unsigned int align,
						     gfp_t gfp_mask)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align >= PAGE_SIZE ||
		     *size < sizeof(unsigned int));

	*offset = nc->offset;
	nc->offset = ALIGN(*offset, align);
	return page_frag_alloc_va_prepare(nc, offset, size, gfp_mask);
}

static inline void *__page_frag_alloc_pg_prepare(struct page_frag_cache *nc,
						 unsigned int *offset,
						 unsigned int *size,
						 gfp_t gfp_mask)
{
	void *va;

	va = page_frag_cache_refill(nc, *size, gfp_mask);
	if (unlikely(!va))
		return NULL;

	*offset = nc->offset;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	*size = nc->size_mask - *offset + 1;
#else
	*size = PAGE_SIZE - *offset;
#endif

	return va;
}

/**
 * page_frag_alloc_pg_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @size: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘size’, 'size' is also used to
 * report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * Return the page fragment, otherwise return NULL.
 */
#define page_frag_alloc_pg_prepare(nc, offset, size, gfp)		\
({									\
	struct page *__page = NULL;					\
	void *__va;							\
									\
	__va = __page_frag_alloc_pg_prepare(nc, offset, size, gfp);	\
	if (likely(__va))						\
		__page = virt_to_page(__va);				\
									\
	__page;								\
})

static inline void *__page_frag_alloc_prepare(struct page_frag_cache *nc,
					      unsigned int *offset,
					      unsigned int *size,
					      void **va, gfp_t gfp_mask)
{
	void *nc_va;

	nc_va = page_frag_cache_refill(nc, *size, gfp_mask);
	if (unlikely(!nc_va))
		return NULL;

	*offset = nc->offset;
	*va = nc_va + *offset;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	*size = nc->size_mask - *offset + 1;
#else
	*size = PAGE_SIZE - *offset;
#endif

	return nc_va;
}

/**
 * page_frag_alloc_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @size: in as the requested size, out as the available size
 * @va: out as the va of the returned page fragment
 * @gfp: the allocation gfp to use when cache need to be refilled
 *
 * Prepare a page fragment with minimum size of ‘size’, 'size' is also used to
 * report the maximum size of the page fragment. Return both 'page' and 'va' of
 * the fragment to the caller.
 *
 * Return:
 * Return the page fragment, otherwise return NULL.
 */
#define page_frag_alloc_prepare(nc, offset, size, va, gfp)		\
({									\
	struct page *__page = NULL;					\
	void *__va;							\
									\
	__va = __page_frag_alloc_prepare(nc, offset, size, va, gfp);	\
	if (likely(__va))						\
		__page = virt_to_page(__va);				\
									\
	__page;								\
})

/**
 * page_frag_alloc_commit - Commit allocing a page fragment.
 * @nc: page_frag cache from which to commit
 * @offset: offset of the page fragment
 * @size: size of the page fragment has been used
 *
 * Commit the alloc preparing by passing offset and the actual used size.
 */
static inline void page_frag_alloc_commit(struct page_frag_cache *nc,
					  unsigned int offset,
					  unsigned int size)
{
	nc->pagecnt_bias--;
	nc->offset = offset + size;
}

/**
 * page_frag_alloc_commit_noref - Commit allocing a page fragment without taking
 * page refcount.
 * @nc: page_frag cache from which to commit
 * @offset: offset of the page fragment
 * @size: size of the page fragment has been used
 *
 * Commit the alloc preparing by passing offset and the actual used size, but
 * not taking page refcount. Mostly used for fragmemt coaleasing case when the
 * current fragmemt can share the same refcount with previous fragmemt.
 */
static inline void page_frag_alloc_commit_noref(struct page_frag_cache *nc,
						unsigned int offset,
						unsigned int size)
{
	nc->offset = offset + size;
}

/**
 * page_frag_free_va - Free a page fragment by va.
 * @addr: va of page fragment to be freed
 */
void page_frag_free_va(void *addr);

#endif
