#ifndef __MM_PAGE_PCP_H
#define __MM_PAGE_PCP_H

#include <linux/mmzone.h>

int decay_pcp_high(struct zone *zone, struct per_cpu_pages *pcp);
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp);
void drain_all_pages(struct zone *zone);

void setup_per_cpu_boot_pageset(void);
int percpu_pagelist_high_fraction_set(int new);
void zone_pcp_update(struct zone *zone, int cpu_online);

bool free_unref_pcp_page(struct zone *zone, struct page *page,
			 unsigned long pfn, unsigned int order);
void free_unref_pcp_folios(struct folio_batch *folios);
struct page *rmqueue_pcplist(struct zone *preferred_zone, struct zone *zone,
			     unsigned int order, int migratetype,
			     unsigned int alloc_flags);
int rmqueue_pcplist_bulk(struct zone *zone, int nr_pages, int migratetype,
			 unsigned int alloc_flags, struct list_head *page_list);
#endif	/* __MM_PAPE_PCP_H */
