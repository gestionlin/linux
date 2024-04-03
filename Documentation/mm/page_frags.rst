.. SPDX-License-Identifier: GPL-2.0

==============
Page fragments
==============

.. kernel-doc:: mm/page_frag_cache.c
   :doc: page_frag allocator

Architecture overview
=====================

.. code-block:: none

    +----------------------+
    | page_frag API caller |
    +----------------------+
            ^
            |
            |
            |
            v
    +----------------------------------------------+
    |          request page fragment               |
    +----------------------------------------------+
        ^                                        ^
        |                                        |
        | Cache empty or not enough              |
        |                                        |
        v                                        |
    +--------------------------------+           |
    | refill cache with order 3 page |           |
    +--------------------------------+           |
     ^                  ^                        |
     |                  |                        |
     |                  | Refill failed          |
     |                  |                        | Cache is enough
     |                  |                        |
     |                  v                        |
     |    +----------------------------------+   |
     |    |  refill cache with order 0 page  |   |
     |    +----------------------------------+   |
     |                       ^                   |
     | Refill succeed        |                   |
     |                       | Refill succeed    |
     |                       |                   |
     v                       v                   v
    +----------------------------------------------+
    |       allocate fragment from cache           |
    +----------------------------------------------+

API interface
=============
As the design and implementation of page_frag API, the allocation side does not
allow concurrent calling, it is assumed that the caller must ensure there is not
concurrent alloc calling to the same page_frag_cache instance by using it's own
lock or rely on some lockless guarantee like NAPI softirq.

Depending on different use cases, callers expecting to deal with va, page or
both va and page for them may call page_frag_alloc_va(), page_frag_alloc_pg(),
or page_frag_alloc() accordingly.

There is also a use case that need minimum memory in order for forward
progressing, but can do better if there is more memory available. Introduce
page_frag_alloc_prepare() and page_frag_alloc_commit() related API, the caller
requests the minimum memory it need and the prepare API will return the maximum
size of the fragment returned, caller need to report back to the page_frag core
how much memory it actually use by calling commit API, or not calling the commit
API if deciding to not use any memory.

.. kernel-doc:: include/linux/page_frag_cache.h
   :identifiers: page_frag_cache_init page_frag_cache_is_pfmemalloc
                 page_frag_alloc_va __page_frag_alloc_va_align
                 page_frag_alloc_va_align page_frag_alloc_va_prepare
                 page_frag_alloc_va_prepare_align page_frag_alloc_pg_prepare
                 page_frag_alloc_prepare page_frag_alloc_commit
                 page_frag_alloc_commit_noref page_frag_free_va

.. kernel-doc:: mm/page_frag_cache.c
   :identifiers: page_frag_cache_drain
