/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Growable shmem bump-allocator pool.  Each pool_alloc returns a
 * refcounted shmem + offset; on grow, in-flight callers' refs keep
 * the old shmem alive until released.
 */

#ifndef NPT_SHMEM_POOL_H
#define NPT_SHMEM_POOL_H

#include "npt_common.h"
#include "npt_renderer.h"

struct npt_shmem_pool {
   mtx_t mutex;
   size_t min_alloc_size;
   struct npt_renderer_shmem *shmem;
   size_t size;
   size_t used;
};

void
npt_shmem_pool_init(struct npt_shmem_pool *pool, size_t min_alloc_size);

void
npt_shmem_pool_fini(struct npt_renderer *renderer,
                    struct npt_shmem_pool *pool);

/*
 * Caller takes the returned shmem ref and must unref when done with
 * the region.  A command naming the shmem's res_id must first pass
 * npt_ring_ensure_shmem_visible on its ring.
 */
struct npt_renderer_shmem *
npt_shmem_pool_alloc(struct npt_renderer *renderer,
                     struct npt_shmem_pool *pool,
                     size_t size, size_t *out_offset);

#endif /* NPT_SHMEM_POOL_H */
