// SPDX-License-Identifier: GPL-2.0-only
/* Page fragment allocator
 *
 * Page Fragment:
 *  An arbitrary-length arbitrary-offset area of memory which resides within a
 *  0 or higher order page.  Multiple fragments within that page are
 *  individually refcounted, in the page's reference counter.
 *
 * The page_frag functions provide a simple allocation framework for page
 * fragments.  This is used by the network stack and network device drivers to
 * provide a backing region of memory for use as either an sk_buff->head, or to
 * be used in the "frags" portion of skb_shared_info.
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_frag_cache.h>
#include "internal.h"

static void *page_frag_cache_current_va(struct page_frag_cache *nc)
{
	struct encoded_va *encoded_va = nc->encoded_va;

	return (void *)(((unsigned long)encoded_va & PAGE_MASK) |
		(page_frag_cache_page_size(encoded_va) - nc->remaining));
}

static struct page *__page_frag_cache_refill(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	struct encoded_va *encoded_va = nc->encoded_va;
	gfp_t gfp = gfp_mask;
	unsigned int order;
	struct page *page;

	if (unlikely(!encoded_va))
		goto alloc;

	page = virt_to_page(encoded_va);
	if (!page_ref_sub_and_test(page, nc->pagecnt_bias))
		goto alloc;

	if (unlikely(encoded_page_pfmemalloc(encoded_va))) {
		VM_BUG_ON(compound_order(page) !=
			  encoded_page_order(encoded_va));
		free_unref_page(page, encoded_page_order(encoded_va));
		goto alloc;
	}

	/* OK, page count is 0, we can safely set it */
	set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);

	/* reset page count bias and remaining of new frag */
	nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
	nc->remaining = page_frag_cache_page_size(encoded_va);

	return page;

alloc:
	page = NULL;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* Ensure free_unref_page() can be used to free the page fragment */
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_ALLOC_COSTLY_ORDER);

	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = __alloc_pages(gfp_mask, PAGE_FRAG_CACHE_MAX_ORDER,
			     numa_mem_id(), NULL);
#endif
	if (unlikely(!page)) {
		page = __alloc_pages(gfp, 0, numa_mem_id(), NULL);
		if (unlikely(!page)) {
			memset(nc, 0, sizeof(*nc));
			return NULL;
		}

		order = 0;
		nc->remaining = PAGE_SIZE;
	} else {
		order = PAGE_FRAG_CACHE_MAX_ORDER;
		nc->remaining = PAGE_FRAG_CACHE_MAX_SIZE;
	}

	/* Even if we own the page, we do not use atomic_set().
	 * This would break get_page_unless_zero() users.
	 */
	page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);

	/* reset page count bias of new frag */
	nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
	nc->encoded_va = encode_aligned_va(page_address(page), order,
					   page_is_pfmemalloc(page));
	return page;
}

void *page_frag_alloc_va_prepare(struct page_frag_cache *nc,
				 unsigned int *fragsz, gfp_t gfp)
{
	struct encoded_va *encoded_va;
	unsigned int remaining;

	remaining = nc->remaining;
	if (unlikely(*fragsz > remaining)) {
		if (unlikely(!__page_frag_cache_refill(nc, gfp) ||
			     *fragsz > PAGE_SIZE))
			return NULL;

		remaining = nc->remaining;
	}

	encoded_va = nc->encoded_va;
	*fragsz = remaining;
	return encoded_page_address(encoded_va) +
			page_frag_cache_page_size(encoded_va) - remaining;
}
EXPORT_SYMBOL(page_frag_alloc_va_prepare);

struct page *page_frag_alloc_pg_prepare(struct page_frag_cache *nc,
					unsigned int *offset,
					unsigned int *fragsz, gfp_t gfp)
{
	struct encoded_va *encoded_va;
	unsigned int remaining;
	struct page *page;

	remaining = nc->remaining;
	if (unlikely(*fragsz > remaining)) {
		if (unlikely(*fragsz > PAGE_SIZE)) {
			*fragsz = 0;
			return NULL;
		}

		page = __page_frag_cache_refill(nc, gfp);
		remaining = nc->remaining;
		encoded_va = nc->encoded_va;
	} else {
		encoded_va = nc->encoded_va;
		page = virt_to_page(encoded_va);
	}

	*offset = page_frag_cache_page_size(encoded_va) - remaining;
	*fragsz = remaining;

	return page;
}
EXPORT_SYMBOL(page_frag_alloc_pg_prepare);

struct page *page_frag_alloc_prepare(struct page_frag_cache *nc,
				     unsigned int *offset,
				     unsigned int *fragsz,
				     void **va, gfp_t gfp)
{
	struct encoded_va *encoded_va;
	unsigned int remaining;
	struct page *page;

	remaining = nc->remaining;
	if (unlikely(*fragsz > remaining)) {
		if (unlikely(*fragsz > PAGE_SIZE)) {
			*fragsz = 0;
			return NULL;
		}

		page = __page_frag_cache_refill(nc, gfp);
		remaining = nc->remaining;
		encoded_va = nc->encoded_va;
	} else {
		encoded_va = nc->encoded_va;
		page = virt_to_page(encoded_va);
	}

	*offset = page_frag_cache_page_size(encoded_va) - remaining;
	*fragsz = remaining;
	*va = encoded_page_address(encoded_va) + *offset;

	return page;
}
EXPORT_SYMBOL(page_frag_alloc_prepare);

struct page *page_frag_alloc_pg(struct page_frag_cache *nc,
				unsigned int *offset, unsigned int fragsz,
				gfp_t gfp)
{
	struct page *page;

	if (unlikely(fragsz > nc->remaining)) {
		if (unlikely(fragsz > PAGE_SIZE))
			return NULL;

		page = __page_frag_cache_refill(nc, gfp);
		if (unlikely(!page))
			return NULL;

		*offset = 0;
	} else {
		struct encoded_va *encoded_va = nc->encoded_va;

		page = virt_to_page(encoded_va);
		*offset = page_frag_cache_page_size(encoded_va) -
					nc->remaining;
	}

	nc->remaining -= fragsz;
	nc->pagecnt_bias--;

	return page;
}
EXPORT_SYMBOL(page_frag_alloc_pg);

void page_frag_cache_drain(struct page_frag_cache *nc)
{
	if (!nc->encoded_va)
		return;

	__page_frag_cache_drain(virt_to_head_page(nc->encoded_va),
				nc->pagecnt_bias);
	memset(nc, 0, sizeof(*nc));
}
EXPORT_SYMBOL(page_frag_cache_drain);

void __page_frag_cache_drain(struct page *page, unsigned int count)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);

	if (page_ref_sub_and_test(page, count))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(__page_frag_cache_drain);

void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
				 unsigned int fragsz, gfp_t gfp_mask,
				 unsigned int align_mask)
{
	int remaining = nc->remaining & align_mask;
	void *va;

	remaining -= fragsz;
	if (unlikely(remaining < 0)) {
		if (unlikely(!__page_frag_cache_refill(nc, gfp_mask)))
			return NULL;

		remaining = nc->remaining - fragsz;
		if (unlikely(remaining < 0)) {
			/*
			 * The caller is trying to allocate a fragment
			 * with fragsz > PAGE_SIZE but the cache isn't big
			 * enough to satisfy the request, this may
			 * happen in low memory conditions.
			 * We don't release the cache page because
			 * it could make memory pressure worse
			 * so we simply return NULL here.
			 */
			return NULL;
		}
	}

	va = page_frag_cache_current_va(nc);
	nc->pagecnt_bias--;
	nc->remaining = remaining;

	return va;
}
EXPORT_SYMBOL(__page_frag_alloc_va_align);

/*
 * Frees a page fragment allocated out of either a compound or order 0 page.
 */
void page_frag_free_va(void *addr)
{
	struct page *page = virt_to_head_page(addr);

	if (unlikely(put_page_testzero(page)))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(page_frag_free_va);
