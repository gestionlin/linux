/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/gfp.h>

#define PAGE_FRAG_CACHE_MAX_SIZE	__ALIGN_MASK(32768, ~PAGE_MASK)
#define PAGE_FRAG_CACHE_MAX_ORDER	get_order(PAGE_FRAG_CACHE_MAX_SIZE)

struct page_frag_cache {
	/* page address and offset */
	void *va;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE) && (BITS_PER_LONG <= 32)
	u16 pagecnt_bias;
	u16 size_mask:15;
	u16 pfmemalloc:1;
#else
	u32 pagecnt_bias;
	u16 size_mask;
	u16 pfmemalloc;
#endif
};

static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->va = NULL;
}

static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return !!nc->pfmemalloc;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
				 unsigned int fragsz, gfp_t gfp_mask,
				 unsigned int align_mask);

static inline void *page_frag_alloc_va_align(struct page_frag_cache *nc,
					     unsigned int fragsz,
					     gfp_t gfp_mask, unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_va_align(nc, fragsz, gfp_mask, -align);
}

static inline void *page_frag_alloc_va(struct page_frag_cache *nc,
				       unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_va_align(nc, fragsz, gfp_mask, ~0u);
}

void page_frag_free_va(void *addr);

#endif
