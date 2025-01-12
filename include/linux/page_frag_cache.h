/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/bits.h>
#include <linux/log2.h>
#include <linux/mm_types_task.h>
#include <linux/types.h>

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
/* Use a full byte here to enable assembler optimization as the shift
 * operation is usually expecting a byte.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(0, 0)
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

#if 0
static inline unsigned long encoded_page_decode_order(unsigned long encoded_page)
{
	static unsigned int orders[2] = {0, ilog2(PAGE_FRAG_CACHE_MAX_SIZE / PAGE_SIZE)};
        return orders[encoded_page & PAGE_FRAG_CACHE_ORDER_MASK];
}
#endif

static inline unsigned long encoded_page_decode_size(unsigned long encoded_page)
{
        static unsigned int sizes[2] = {PAGE_SIZE, PAGE_FRAG_CACHE_MAX_SIZE};
        return sizes[encoded_page & PAGE_FRAG_CACHE_ORDER_MASK];
}

static inline void *encoded_page_decode_virt(unsigned long encoded_page)
{
        return (void *)(encoded_page & PAGE_MASK);
}

static inline struct page *encoded_page_decode_page(unsigned long encoded_page)
{
        return virt_to_page((void *)encoded_page);
}

static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return encoded_page_decode_pfmemalloc(nc->encoded_page);
}

static inline struct page *page_frag_cache_page(struct page_frag_cache *nc)
{
	return encoded_page_decode_page(nc->encoded_page);
}

static inline unsigned int page_frag_cache_remaining(struct page_frag_cache *nc)
{
	return encoded_page_decode_size(nc->encoded_page) - nc->offset;
}

static inline unsigned int page_frag_cache_offset(struct page_frag_cache *nc)
{
	return nc->offset;
}

static inline void *page_frag_cache_virt(struct page_frag_cache *nc)
{
	return encoded_page_decode_virt(nc->encoded_page) + nc->offset;
}

static inline void page_frag_cache_commit_noref(struct page_frag_cache *nc,
	      					unsigned int fragsz)
{
	nc->offset += fragsz;
}

static inline void page_frag_cache_commit(struct page_frag_cache *nc,
					  unsigned int fragsz)
{
	page_frag_cache_commit_noref(nc, fragsz);
	nc->pagecnt_bias--;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_alloc_align(struct page_frag_cache *nc, unsigned int fragsz,
			      gfp_t gfp_mask, unsigned int align_mask);

bool __page_frag_cache_refill_align(struct page_frag_cache *nc,
                                   unsigned int fragsz, gfp_t gfp_mask,
                                   unsigned int align_mask);

static inline void *page_frag_alloc_align(struct page_frag_cache *nc,
					  unsigned int fragsz, gfp_t gfp_mask,
					  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, -align);
}

static inline void *page_frag_alloc(struct page_frag_cache *nc,
				    unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

static inline bool page_frag_cache_refill(struct page_frag_cache *nc,
				     unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_cache_refill_align(nc, fragsz, gfp_mask, ~0u);
}

static inline bool page_frag_cache_refill_align(struct page_frag_cache *nc,
					   unsigned int fragsz, gfp_t gfp_mask,
					   unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_cache_refill_align(nc, fragsz, gfp_mask, -align);
}

void page_frag_free(void *addr);

#endif
