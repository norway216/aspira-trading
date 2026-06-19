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

/* mempool_alloc() and mempool_free() are defined as static inline in
 * memory_pool.h for zero-overhead inlining in the hot path. */

uint32_t mempool_available(const mempool_t *mp) {
    if (!mp) return 0;
    return __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
}

uint32_t mempool_capacity(const mempool_t *mp) {
    return mp ? mp->capacity : 0;
}
