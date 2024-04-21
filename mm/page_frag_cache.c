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

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* Ensure free_unref_page() can be used to free the page fragment */
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_ALLOC_COSTLY_ORDER);

	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = alloc_pages_node(NUMA_NO_NODE, gfp_mask,
				PAGE_FRAG_CACHE_MAX_ORDER);
	if (unlikely(!page)) {
		page = alloc_pages_node(NUMA_NO_NODE, gfp, 0);
		nc->size_mask = PAGE_SIZE - 1;
	} else {
		nc->size_mask = PAGE_FRAG_CACHE_MAX_SIZE - 1;
		VM_BUG_ON(BITS_PER_LONG <= 32 &&
			  nc->size_mask != PAGE_FRAG_CACHE_MAX_SIZE - 1);
	}
#else
	page = alloc_pages_node(NUMA_NO_NODE, gfp, 0);
#endif

	nc->va = page ? page_address(page) : NULL;

	return page;
}

void page_frag_cache_drain(struct page_frag_cache *nc)
{
	if (!nc->va)
		return;

	__page_frag_cache_drain(virt_to_head_page(nc->va), nc->pagecnt_bias);
	nc->va = NULL;
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
	unsigned long size_mask, offset;
	struct page *page;
	void *va;

	if (unlikely(!nc->va)) {
refill:
		page = __page_frag_cache_refill(nc, gfp_mask);
		if (!page)
			return NULL;

		/* Even if we own the page, we do not use atomic_set().
		 * This would break get_page_unless_zero() users.
		 */
		page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);

		/* reset page count bias and offset to start of new frag */
		nc->pfmemalloc = page_is_pfmemalloc(page);
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
	}

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* if size_mask can vary, use size_mask else just use PAGE_SIZE - 1 */
	size_mask = nc->size_mask;
#else
	size_mask = PAGE_SIZE - 1;
#endif

	va = nc->va;
	offset = (unsigned long)va & size_mask;
	va = (void *)((unsigned long)va & ~size_mask);
	offset = __ALIGN_KERNEL_MASK(offset, ~align_mask);
	if (unlikely(offset + fragsz >= size_mask + 1)) {
		/* fragsz is not supposed to be bigger than PAGE_SIZE as we are
		 * allowing order 3 page allocation to fail easily under low
		 * memory condition.
		 */
		if (WARN_ON_ONCE(fragsz > PAGE_SIZE))
			return NULL;

		page = virt_to_page(va);

		if (!page_ref_sub_and_test(page, nc->pagecnt_bias))
			goto refill;

		if (unlikely(nc->pfmemalloc)) {
			free_unref_page(page, compound_order(page));
			goto refill;
		}

		/* OK, page count is 0, we can safely set it */
		set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);

		/* reset page count bias and offset to start of new frag */
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		offset = 0;
	}

	nc->pagecnt_bias--;
	va += offset;
	nc->va = va + fragsz;

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
