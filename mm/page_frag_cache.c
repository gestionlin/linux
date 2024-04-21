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

static struct page *__page_frag_cache_refill(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	unsigned int size, order;
	gfp_t gfp = gfp_mask;
	struct page *page;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* Ensure free_unref_page() can be used to free the page fragment */
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_ALLOC_COSTLY_ORDER);

	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = alloc_pages_node(NUMA_NO_NODE, gfp_mask,
				PAGE_FRAG_CACHE_MAX_ORDER);
	if (likely(page)) {
		size = PAGE_FRAG_CACHE_MAX_SIZE;
		order = PAGE_FRAG_CACHE_MAX_ORDER;
		goto out;
	}
#endif
	page = alloc_pages_node(NUMA_NO_NODE, gfp, 0);
	if (unlikely(!page)) {
		nc->encoded_va = NULL;
		nc->remaining = 0;
		return NULL;
	}

	size = PAGE_SIZE;
	order = 0;
out:
	nc->encoded_va = encode_aligned_va(page_address(page), order,
					   page_is_pfmemalloc(page));
	nc->remaining = size;
	page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);
	nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;

	return page;
}

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
	unsigned int remaining, page_size;
	struct encoded_va *encoded_va;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	unsigned long page_order;
#endif
	struct page *page;

alloc_fragment:
	remaining = nc->remaining & align_mask;
	encoded_va = nc->encoded_va;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	page_order = encoded_page_order(encoded_va);
	page_size = PAGE_SIZE << page_order;
#else
	page_size = PAGE_SIZE;
#endif

	if (unlikely(fragsz > remaining)) {
		if (unlikely(!encoded_va)) {
			page = __page_frag_cache_refill(nc, gfp_mask);
			if (page)
				goto alloc_fragment;

			return NULL;
		}

		/* fragsz is not supposed to be bigger than PAGE_SIZE as we are
		 * allowing order 3 page allocation to fail easily under low
		 * memory condition.
		 */
		if (WARN_ON_ONCE(fragsz > PAGE_SIZE))
			return NULL;

		page = virt_to_page(encoded_va);
		if (!page_ref_sub_and_test(page, nc->pagecnt_bias)) {
			page = __page_frag_cache_refill(nc, gfp_mask);
			if (page)
				goto alloc_fragment;

			return NULL;
		}

		if (unlikely(encoded_page_pfmemalloc(encoded_va))) {
			free_unref_page(page, compound_order(page));
			page = __page_frag_cache_refill(nc,  gfp_mask);
			if (page)
				goto alloc_fragment;

			return NULL;
		}

		/* OK, page count is 0, we can safely set it */
		set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);

		/* reset page count bias and offset to start of new frag */
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		remaining = page_size;
	}

	nc->remaining = remaining - fragsz;
	nc->pagecnt_bias--;

	return encoded_page_address(encoded_va) + (page_size - remaining);
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
