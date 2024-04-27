/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/gfp.h>

#define PAGE_FRAG_CACHE_MAX_SIZE	__ALIGN_MASK(32768, ~PAGE_MASK)
#define PAGE_FRAG_CACHE_MAX_ORDER	get_order(PAGE_FRAG_CACHE_MAX_SIZE)

#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(1, 0)
#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		BIT(2)
#define PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT	2

struct page_frag_cache {
	/* page address & pfmemalloc & order */
	void *va;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE) && (BITS_PER_LONG <= 32)
	u16 pagecnt_bias;
	u16 size;
#else
	u32 pagecnt_bias;
	u32 size;
#endif
};

static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	memset(nc, 0, sizeof(*nc));
}

static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return (unsigned long)nc->va & PAGE_FRAG_CACHE_PFMEMALLOC_BIT;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);

struct page *__page_frag_cache_refill(struct page_frag_cache *nc, gfp_t gfp);

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

static inline unsigned int page_frag_cache_page_offset(struct page_frag_cache *nc)
{
	return __page_frag_cache_page_offset(nc->va, nc->size);
}

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

static inline void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
					       unsigned int fragsz, gfp_t gfp,
					       unsigned int align_mask)
{
	nc->size = nc->size & align_mask;
	return page_frag_alloc_va(nc, fragsz, gfp);
}

static inline void *page_frag_alloc_va_align(struct page_frag_cache *nc,
					     unsigned int fragsz,
					     gfp_t gfp, unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align > PAGE_SIZE);
	return __page_frag_alloc_va_align(nc, fragsz, gfp, -align);
}

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

static inline void *page_frag_alloc_va_prepare_align(struct page_frag_cache *nc,
						     unsigned int *fragsz,
						     gfp_t gfp,
						     unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align) || align > PAGE_SIZE);
	nc->size = nc->size & -align;
	return page_frag_alloc_va_prepare(nc, fragsz, gfp);
}

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

static inline void page_frag_alloc_commit(struct page_frag_cache *nc,
					  unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->size || !nc->pagecnt_bias);
	nc->pagecnt_bias--;
	nc->size -= fragsz;
}

static inline void page_frag_alloc_commit_noref(struct page_frag_cache *nc,
						unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->size);
	nc->size -= fragsz;
}

void page_frag_free_va(void *addr);

#endif
