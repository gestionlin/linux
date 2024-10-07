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

static inline bool page_frag_encoded_page_pfmemalloc(unsigned long encoded_page)
{
	return !!(encoded_page & PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return page_frag_encoded_page_pfmemalloc(nc->encoded_page);
}

static inline unsigned int page_frag_cache_page_offset(const struct page_frag_cache *nc)
{
	return nc->offset;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_cache_prepare(struct page_frag_cache *nc, unsigned int fragsz,
				struct page_frag *pfrag, gfp_t gfp_mask,
				unsigned int align_mask);
unsigned int __page_frag_cache_commit_noref(struct page_frag_cache *nc,
					    struct page_frag *pfrag,
					    unsigned int used_sz);
void *__page_frag_alloc_refill_probe_align(struct page_frag_cache *nc,
					   unsigned int fragsz,
					   struct page_frag *pfrag,
					   unsigned int align_mask);

static inline unsigned int __page_frag_cache_commit(struct page_frag_cache *nc,
						    struct page_frag *pfrag,
						    unsigned int used_sz)
{
	VM_BUG_ON(!nc->pagecnt_bias);
	nc->pagecnt_bias--;

	return __page_frag_cache_commit_noref(nc, pfrag, used_sz);
}

static inline void *__page_frag_alloc_align(struct page_frag_cache *nc,
					    unsigned int fragsz, gfp_t gfp_mask,
					    unsigned int align_mask)
{
	struct page_frag page_frag;
	void *va;

	va = __page_frag_cache_prepare(nc, fragsz, &page_frag, gfp_mask,
				       align_mask);
	if (unlikely(!va))
		return NULL;

	__page_frag_cache_commit(nc, &page_frag, fragsz);

	return va;
}

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

static inline bool __page_frag_refill_align(struct page_frag_cache *nc,
					    unsigned int fragsz,
					    struct page_frag *pfrag,
					    gfp_t gfp_mask,
					    unsigned int align_mask)
{
	if (unlikely(!__page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask,
						align_mask)))
		return false;

	__page_frag_cache_commit(nc, pfrag, fragsz);
	return true;
}

static inline bool page_frag_refill_align(struct page_frag_cache *nc,
					  unsigned int fragsz,
					  struct page_frag *pfrag,
					  gfp_t gfp_mask, unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_refill_align(nc, fragsz, pfrag, gfp_mask, -align);
}

static inline bool page_frag_refill(struct page_frag_cache *nc,
				    unsigned int fragsz,
				    struct page_frag *pfrag, gfp_t gfp_mask)
{
	return __page_frag_refill_align(nc, fragsz, pfrag, gfp_mask, ~0u);
}

static inline bool __page_frag_refill_prepare_align(struct page_frag_cache *nc,
						    unsigned int fragsz,
						    struct page_frag *pfrag,
						    gfp_t gfp_mask,
						    unsigned int align_mask)
{
	return !!__page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask,
					   align_mask);
}

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

static inline bool page_frag_refill_prepare(struct page_frag_cache *nc,
					    unsigned int fragsz,
					    struct page_frag *pfrag,
					    gfp_t gfp_mask)
{
	return __page_frag_refill_prepare_align(nc, fragsz, pfrag, gfp_mask,
						~0u);
}

static inline void *__page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
							   unsigned int fragsz,
							   struct page_frag *pfrag,
							   gfp_t gfp_mask,
							   unsigned int align_mask)
{
	return __page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask, align_mask);
}

static inline void *page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
							 unsigned int fragsz,
							 struct page_frag *pfrag,
							 gfp_t gfp_mask,
							 unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag,
						      gfp_mask, -align);
}

static inline void *page_frag_alloc_refill_prepare(struct page_frag_cache *nc,
						   unsigned int fragsz,
						   struct page_frag *pfrag,
						   gfp_t gfp_mask)
{
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag,
						      gfp_mask, ~0u);
}

static inline void *page_frag_alloc_refill_probe(struct page_frag_cache *nc,
						 unsigned int fragsz,
						 struct page_frag *pfrag)
{
	return __page_frag_alloc_refill_probe_align(nc, fragsz, pfrag, ~0u);
}

static inline bool page_frag_refill_probe(struct page_frag_cache *nc,
					  unsigned int fragsz,
					  struct page_frag *pfrag)
{
	return !!page_frag_alloc_refill_probe(nc, fragsz, pfrag);
}

static inline void page_frag_commit(struct page_frag_cache *nc,
				    struct page_frag *pfrag,
				    unsigned int used_sz)
{
	__page_frag_cache_commit(nc, pfrag, used_sz);
}

static inline void page_frag_commit_noref(struct page_frag_cache *nc,
					  struct page_frag *pfrag,
					  unsigned int used_sz)
{
	__page_frag_cache_commit_noref(nc, pfrag, used_sz);
}

static inline void page_frag_alloc_abort(struct page_frag_cache *nc,
					 unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->offset);

	nc->pagecnt_bias++;
	nc->offset -= fragsz;
}

void page_frag_free(void *addr);

#endif
