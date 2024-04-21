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
	gfp_t gfp = gfp_mask;
	struct page *page;
	void *va;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* Ensure free_unref_page() can be used to free the page fragment */
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_ALLOC_COSTLY_ORDER);

	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = alloc_pages_node(NUMA_NO_NODE, gfp_mask,
				PAGE_FRAG_CACHE_MAX_ORDER);
	if (likely(page)) {
		nc->size = PAGE_FRAG_CACHE_MAX_SIZE;
		va = page_address(page);
		nc->va = (void *)((unsigned long)va |
				  PAGE_FRAG_CACHE_MAX_ORDER |
				  (page_is_pfmemalloc(page) <<
				   PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT));
		page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		return page;
	}
#endif
	page = alloc_pages_node(NUMA_NO_NODE, gfp, 0);
	if (likely(page)) {
		nc->size = PAGE_SIZE;
		va = page_address(page);
		nc->va = (void *)((unsigned long)va |
				  (page_is_pfmemalloc(page) <<
				   PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT));
		page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		return page;
	}

	nc->va = NULL;
	nc->size = 0;
	return NULL;
}

void page_frag_cache_drain(struct page_frag_cache *nc)
{
	if (!nc->va)
		return;

	__page_frag_cache_drain(virt_to_head_page(nc->va), nc->pagecnt_bias);
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
	unsigned int size, page_size;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	unsigned long page_order;
#endif
	struct page *page;
	void *va;

alloc_fragment:
	size = nc->size & align_mask;
	va = nc->va;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	page_order = (unsigned long)va & PAGE_FRAG_CACHE_ORDER_MASK;
	page_size = PAGE_SIZE << page_order;
#else
	page_size = PAGE_SIZE;
#endif

	if (unlikely(fragsz > size)) {
		if (unlikely(!va)) {
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

		page = virt_to_page(va);
		if (!page_ref_sub_and_test(page, nc->pagecnt_bias)) {
			page = __page_frag_cache_refill(nc, gfp_mask);
			if (page)
				goto alloc_fragment;

			return NULL;
		}

		if (unlikely((unsigned long)va &
			     PAGE_FRAG_CACHE_PFMEMALLOC_BIT)) {
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
		size = page_size;
	}

	va = (void *)((unsigned long)va & PAGE_MASK);
	va = va + (page_size - size);
	nc->size = size - fragsz;
	nc->pagecnt_bias--;

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
