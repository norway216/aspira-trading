/**
 * memory_pool.c — Pre-allocated Memory Pool Implementation
 *
 * Thread-safe via GCC __atomic builtins.
 *
 * Design:
 *   free:  fetch_add(free_top, RELAXED) → atomic_store(free_list[slot], RELEASE)
 *   alloc: fetch_sub(free_top, RELAXED) → atomic_load(free_list[old_top-1], ACQUIRE)
 *
 * The fetch_add/fetch_sub on free_top atomically claim exclusive slots
 * (eliminating the original TOCTOU race in free). The RELEASE/ACQUIRE
 * pair on the same free_list element guarantees the written index is
 * visible to the consumer.
 */
#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>

int mempool_init(mempool_t *mp, size_t object_size, uint32_t capacity) {
    if (!mp || object_size == 0 || capacity == 0) return -1;

    if (object_size < sizeof(uint32_t)) {
        object_size = sizeof(uint32_t);
    }

    mp->pool = calloc(capacity, object_size);
    if (!mp->pool) return -1;

    mp->free_list = (uint32_t *)malloc(capacity * sizeof(uint32_t));
    if (!mp->free_list) {
        free(mp->pool);
        mp->pool = NULL;
        return -1;
    }

    mp->object_size = (uint32_t)object_size;
    mp->capacity = capacity;

    /* Pre-fill the free list: indices 0..capacity-1 in reverse order.
     * LIFO allocation is cache-friendly (reuses recently-freed objects). */
    for (uint32_t i = 0; i < capacity; i++) {
        mp->free_list[i] = capacity - 1 - i;
    }
    mp->free_top = capacity;

    return 0;
}

void mempool_destroy(mempool_t *mp) {
    if (!mp) return;
    free(mp->pool);
    free(mp->free_list);
    mp->pool = NULL;
    mp->free_list = NULL;
    mp->capacity = 0;
    mp->object_size = 0;
}

void *mempool_alloc(mempool_t *mp) {
    if (!mp) return NULL;

    /* Atomically claim a free slot.
     * __ATOMIC_RELAXED is sufficient — we don't need ordering from
     * free_top itself; the ACQUIRE on the free_list load provides
     * the necessary synchronization with free's RELEASE store. */
    uint32_t old_top = __atomic_fetch_sub(&mp->free_top, 1, __ATOMIC_RELAXED);
    if (old_top == 0) {
        /* Pool exhausted — undo the decrement */
        __atomic_fetch_add(&mp->free_top, 1, __ATOMIC_RELAXED);
        return NULL;
    }

    /* old_top was in [1, capacity]. Read the index from free_list.
     * __ATOMIC_ACQUIRE pairs with the __ATOMIC_RELEASE store in
     * mempool_free, guaranteeing we see the index written by the
     * thread that freed this slot. */
    uint32_t idx = __atomic_load_n(&mp->free_list[old_top - 1], __ATOMIC_ACQUIRE);
    return (char *)mp->pool + (size_t)idx * mp->object_size;
}

void mempool_free(mempool_t *mp, void *ptr) {
    if (!mp || !ptr) return;

    /* Compute the index of this pointer within the pool */
    uintptr_t offset = (uintptr_t)((char *)ptr - (char *)mp->pool);
    uint32_t idx = (uint32_t)(offset / mp->object_size);

    if (idx >= mp->capacity) {
        return; /* Invalid pointer — not from this pool */
    }

    /* Atomically claim the next free_list write slot.
     * fetch_add guarantees each calling thread gets a unique slot,
     * eliminating the TOCTOU race present in the original code. */
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

uint32_t mempool_available(const mempool_t *mp) {
    if (!mp) return 0;
    return __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
}

uint32_t mempool_capacity(const mempool_t *mp) {
    return mp ? mp->capacity : 0;
}
