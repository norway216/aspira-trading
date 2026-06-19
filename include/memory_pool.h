/**
 * memory_pool.h — Pre-allocated Fixed-Size Memory Pool
 *
 * Allocates a slab of objects upfront. Allocation is O(1) stack-pop;
 * deallocation is O(1) stack-push. No malloc/free in the hot path.
 * Thread-safe via GCC __atomic builtins (C/C++ portable).
 */
#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMPOOL_CACHE_LINE 64

/**
 * A thread-safe, pre-allocated fixed-size memory pool.
 *
 * Uses an atomic free-list (stack). Each freed slot stores the index
 * of the next free slot in its first bytes.
 * GCC __atomic builtins for C/C++ cross-language compatibility.
 */
typedef struct {
    /* free_top is the only hot field (atomic via multiple threads).
     * Placed first on its own cache line to avoid false sharing
     * with read-mostly fields below. */
    uint32_t free_top;     /* top of free stack (atomic via __atomic builtins) */
    char _pad1[MEMPOOL_CACHE_LINE - sizeof(uint32_t)];

    /* Read-mostly fields — only written during init, read during operation */
    void *pool;            /* contiguous block of objects */
    uint32_t object_size;  /* size of each object */
    uint32_t capacity;     /* total number of objects */
    uint32_t *free_list;   /* array of indices (the stack) */
    char _pad2[4];         /* align to 8 bytes for free_list pointer */
} mempool_t;

/**
 * Initialize a memory pool.
 * @param mp          Pointer to uninitialized mempool_t
 * @param object_size Size of each object in bytes
 * @param capacity    Number of objects to pre-allocate
 * @return 0 on success, -1 on error
 */
int mempool_init(mempool_t *mp, size_t object_size, uint32_t capacity);

/**
 * Destroy the pool and free all memory.
 */
void mempool_destroy(mempool_t *mp);

/**
 * Allocate one object from the pool — O(1), static inline (hot path).
 * Returns NULL if the pool is exhausted.
 * Safe for single-producer or multi-producer with __ATOMIC_RELAXED fetch_sub.
 */
static inline void *mempool_alloc(mempool_t *mp) {
    /* Atomically claim a free slot via fetch_sub on free_top.
     * RELAXED sufficient for the counter: atomicity is all we need from
     * free_top itself — the ACQUIRE on the free_list load pairs with
     * the RELEASE store in mempool_free, guaranteeing we see the index
     * written by the thread that freed this slot. */
    uint32_t old_top = __atomic_fetch_sub(&mp->free_top, 1, __ATOMIC_RELAXED);
    if (old_top == 0) {
        /* Pool exhausted — undo the decrement */
        __atomic_fetch_add(&mp->free_top, 1, __ATOMIC_RELAXED);
        return NULL;
    }

    /* old_top was in [1, capacity]. Read the index with ACQUIRE ordering
     * to pair with the RELEASE store in mempool_free. Without this barrier,
     * we may see a stale free_list entry from before the freeing thread's
     * store, resulting in a data race (UB) and potential double-allocation. */
    uint32_t idx = __atomic_load_n(&mp->free_list[old_top - 1], __ATOMIC_ACQUIRE);
    return (char *)mp->pool + (size_t)idx * mp->object_size;
}

/**
 * Return an object to the pool — O(1), static inline (hot path).
 * Thread-safe for both single and multi-consumer free via fetch_add.
 *
 * Uses __atomic_fetch_add to atomically claim an exclusive free_list slot,
 * eliminating the TOCTOU race present in the original load-then-store design.
 * The RELEASE store on the slot guarantees the index is visible to any
 * subsequent alloc's ACQUIRE load.
 */
static inline void mempool_free(mempool_t *mp, void *ptr) {
    uintptr_t offset = (uintptr_t)((char *)ptr - (char *)mp->pool);
    uint32_t idx = (uint32_t)(offset / mp->object_size);
    if (idx >= mp->capacity) return; /* Not from this pool */

    /* Atomically claim the next free_list write slot.
     * fetch_add guarantees each calling thread gets a unique slot,
     * eliminating the TOCTOU race. */
    uint32_t slot = __atomic_fetch_add(&mp->free_top, 1, __ATOMIC_RELAXED);
    if (slot >= mp->capacity) {
        /* Pool at capacity — indicates double-free or use-after-free.
         * Undo the reservation and bail out silently. */
        __atomic_fetch_sub(&mp->free_top, 1, __ATOMIC_RELAXED);
        return;
    }

    /* Write the freed index to our exclusively-owned slot.
     * __ATOMIC_RELEASE ensures this write is globally visible BEFORE
     * any subsequent alloc's ACQUIRE load reads this slot. */
    __atomic_store_n(&mp->free_list[slot], idx, __ATOMIC_RELEASE);
}

/**
 * Return the number of free objects remaining (snapshot).
 */
uint32_t mempool_available(const mempool_t *mp);

/**
 * Return the total capacity of the pool.
 */
uint32_t mempool_capacity(const mempool_t *mp);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POOL_H */
