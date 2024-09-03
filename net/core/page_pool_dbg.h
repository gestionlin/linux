/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Debugging header for page_pool
 *
 *  Copyright (C) 2024 Yunsheng Lin <linyunsheng@huawei.com>
 */

#ifndef __PAGE_POOL_DBG_H
#define __PAGE_POOL_DBG_H

#include <asm-generic/bug.h>
#include <linux/spinlock.h>
#include <net/page_pool/types.h>

#ifdef CONFIG_PAGE_POOL_DEBUG

static inline void __page_pool_debug_alloc_lock(struct page_pool *pool,
						bool allow_direct,
						bool warn_on_destroy)
	__acquires(&pool->ring.consumer_lock)
{
	if (!allow_direct)
		return;

	WARN_ON_ONCE(spin_is_locked(&pool->ring.consumer_lock));
	spin_lock(&pool->ring.consumer_lock);
	WARN_ON_ONCE(warn_on_destroy && pool->destroy_cnt);
}

static inline void __page_pool_debug_alloc_unlock(struct page_pool *pool,
						  bool allow_direct,
						  bool warn_on_destroy)
	__releases(&pool->ring.consumer_lock)
{
	if (!allow_direct)
		return;

	WARN_ON_ONCE(warn_on_destroy && pool->destroy_cnt);
	spin_unlock(&(pool)->ring.consumer_lock);
}

static inline void page_pool_debug_alloc_lock(struct page_pool *pool,
					      bool allow_direct)
	__acquires(&pool->ring.consumer_lock)
{
	return __page_pool_debug_alloc_lock(pool, allow_direct, true);
}

static inline void page_pool_debug_alloc_unlock(struct page_pool *pool,
						bool allow_direct)
	__releases(&pool->ring.consumer_lock)
{
	return __page_pool_debug_alloc_unlock(pool, allow_direct, true);
}

#else

static inline void __page_pool_debug_alloc_lock(struct page_pool *pool,
						bool allow_direct,
						bool warn_on_destroy)
{
}

static inline void __page_pool_debug_alloc_unlock(struct page_pool *pool,
						  bool allow_direct,
						  bool warn_on_destroy)
{
}

static inline void page_pool_debug_alloc_lock(struct page_pool *pool,
					      bool allow_direct)
{
}

static inline void page_pool_debug_alloc_unlock(struct page_pool *pool,
						bool allow_direct)
{
}

#endif

#endif
