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

#include <linux/build_bug.h>
#include <linux/export.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_frag_cache.h>
#include "internal.h"

static unsigned long encoded_page_create(struct page *page, unsigned int order,
					 bool pfmemalloc)
{
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_FRAG_CACHE_ORDER_MASK);
	BUILD_BUG_ON(PAGE_FRAG_CACHE_PFMEMALLOC_BIT >= PAGE_SIZE);

	return (unsigned long)page_address(page) |
		(order & PAGE_FRAG_CACHE_ORDER_MASK) |
		((unsigned long)pfmemalloc * PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

static void *encoded_page_decode_virt(unsigned long encoded_page)
{
	return (void *)(encoded_page & PAGE_MASK);
}

static struct page *__page_frag_cache_refill(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	unsigned long order = PAGE_FRAG_CACHE_MAX_ORDER;
	struct page *page = NULL;
	gfp_t gfp = gfp_mask;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = __alloc_pages(gfp_mask, PAGE_FRAG_CACHE_MAX_ORDER,
			     numa_mem_id(), NULL);
#endif
	if (unlikely(!page)) {
		page = __alloc_pages(gfp, 0, numa_mem_id(), NULL);
		order = 0;
	}

	nc->encoded_page = page ?
		encoded_page_create(page, order, page_is_pfmemalloc(page)) : 0;

	return page;
}

/**
 * page_frag_cache_drain - Drain the current page from page_frag cache.
 * @nc: page_frag cache from which to drain
 */
void page_frag_cache_drain(struct page_frag_cache *nc)
{
	if (!nc->encoded_page)
		return;

	__page_frag_cache_drain(encoded_page_decode_page(nc->encoded_page),
				nc->pagecnt_bias);
	nc->encoded_page = 0;
}
EXPORT_SYMBOL(page_frag_cache_drain);

void __page_frag_cache_drain(struct page *page, unsigned int count)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);

	if (page_ref_sub_and_test(page, count))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(__page_frag_cache_drain);

static void *__page_frag_cache_prepare(struct page_frag_cache *nc,
				       unsigned int fragsz, gfp_t gfp_mask,
				       unsigned int align_mask)
{
	unsigned long encoded_page = nc->encoded_page;
	struct page *page;

	if (likely(encoded_page)) {
		unsigned int size, offset;

		size = PAGE_SIZE << encoded_page_decode_order(encoded_page);
		offset = __ALIGN_KERNEL_MASK(nc->offset, ~align_mask);
		if (likely(offset + fragsz <= size)) {
			nc->offset = offset;
			return encoded_page_decode_virt(encoded_page) + offset;
		}

		page = encoded_page_decode_page(encoded_page);
		if (!page_ref_sub_and_test(page, nc->pagecnt_bias))
			goto refill;

		if (unlikely(encoded_page_decode_pfmemalloc(encoded_page))) {
			free_unref_page(page,
					encoded_page_decode_order(encoded_page));
			goto refill;
		}

		/* OK, page count is 0, we can safely set it */
		set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);
		goto out;
	}

refill:
	page = __page_frag_cache_refill(nc, gfp_mask);
	if (unlikely(!page))
		return NULL;

	encoded_page = nc->encoded_page;

	/* Even if we own the page, we do not use atomic_set().
	 * This would break get_page_unless_zero() users.
	 */
	page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);

out:
	/* reset page count bias and offset to start of new frag */
	nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
	nc->offset = 0;

	if (unlikely(fragsz > PAGE_SIZE))
		return NULL;

	return encoded_page_decode_virt(encoded_page);
}


void *page_frag_cache_prepare(struct page_frag_cache *nc, unsigned int fragsz,
			      struct page_frag *pfrag, gfp_t gfp_mask,
			      unsigned int align_mask)
{
        void *va;

        va = __page_frag_cache_prepare(nc, fragsz, gfp_mask, align_mask);
        if (likely(va)) {
                unsigned long encoded_page = nc->encoded_page;

                pfrag->page = encoded_page_decode_page(encoded_page);
                pfrag->size =
                        (PAGE_SIZE << encoded_page_decode_order(encoded_page)) -
                        nc->offset;
                pfrag->offset = nc->offset;
        }

        return va;
}
EXPORT_SYMBOL(page_frag_cache_prepare);

void *__page_frag_alloc_align(struct page_frag_cache *nc, unsigned int fragsz,
			      gfp_t gfp_mask, unsigned int align_mask)
{
        void *va;

        va = __page_frag_cache_prepare(nc, fragsz, gfp_mask, align_mask);
        if (likely(va)) {
                nc->offset += fragsz;
                nc->pagecnt_bias--;
        }

        return va;
}
EXPORT_SYMBOL(__page_frag_alloc_align);

/**
 * __page_frag_alloc_refill_probe_align() - Probe allocating a fragment and
 * refilling a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @align_mask: the requested aligning requirement for the fragment.
 *
 * Probe allocating a fragment and refilling a page_frag from page_frag cache
 * with aligning requirement.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
void *__page_frag_alloc_refill_probe_align(struct page_frag_cache *nc,
					   unsigned int fragsz,
					   struct page_frag *pfrag,
					   unsigned int align_mask)
{
	unsigned long encoded_page = nc->encoded_page;
	unsigned int size, offset;

	size = PAGE_SIZE << encoded_page_decode_order(encoded_page);
	offset = __ALIGN_KERNEL_MASK(nc->offset, ~align_mask);
	if (unlikely(!encoded_page || offset + fragsz > size))
		return NULL;

	pfrag->page = encoded_page_decode_page(encoded_page);
	pfrag->size = size - offset;
	pfrag->offset = offset;

	return encoded_page_decode_virt(encoded_page) + offset;
}
EXPORT_SYMBOL(__page_frag_alloc_refill_probe_align);

/**
 * page_frag_free - Free a page fragment.
 * @addr: va of page fragment to be freed
 *
 * Free a page fragment allocated out of either a compound or order 0 page by
 * virtual address.
 */
void page_frag_free(void *addr)
{
	struct page *page = virt_to_head_page(addr);

	if (unlikely(put_page_testzero(page)))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(page_frag_free);

/**
 * page_frag_alloc_abort_ref - Abort the reference of allocated fragment.
 * @nc: page_frag cache to which the page fragment is aborted back
 * @va: virtual address of page fragment to be aborted
 * @fragsz: size of the page fragment to be aborted
 *
 * It is expected to be called from the same context as the allocation API.
 * Mostly used for error handling cases to abort the reference of allocated
 * fragment if the fragment has been referenced for other usages, to avoid the
 * atomic operation of page_frag_free() API.
 */
void page_frag_alloc_abort_ref(struct page_frag_cache *nc, void *va,
			       unsigned int fragsz)
{
	VM_BUG_ON(va + fragsz !=
		  encoded_page_decode_virt(nc->encoded_page) + nc->offset);

	nc->pagecnt_bias++;
}
EXPORT_SYMBOL(page_frag_alloc_abort_ref);
