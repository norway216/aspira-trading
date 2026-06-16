/**
 * memory_pool.c — Pre-allocated Memory Pool Implementation
 *
 * Thread-safe via GCC __atomic builtins with relaxed ordering.
 * The free_top counter only needs atomicity, not ordering against
 * surrounding memory operations (the free_list writes are ordered
 * by program order + release semantics where needed).
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

    /* Atomically decrement free_top. __ATOMIC_RELAXED is sufficient:
     * we only need atomicity of the counter itself. */
    uint32_t old_top = __atomic_fetch_sub(&mp->free_top, 1, __ATOMIC_RELAXED);
    if (old_top == 0) {
        /* Pool exhausted — undo the decrement */
        __atomic_fetch_add(&mp->free_top, 1, __ATOMIC_RELAXED);
        return NULL;
    }

    /* old_top was in [1, capacity]. Index is at free_list[old_top - 1].
     * The free_list write (by mempool_free) happens-before the free_top
     * increment via __ATOMIC_RELEASE, so this read is safe. */
    uint32_t idx = mp->free_list[old_top - 1];
    return (char *)mp->pool + (size_t)idx * mp->object_size;
}

void mempool_free(mempool_t *mp, void *ptr) {
    if (!mp || !ptr) return;

    uintptr_t offset = (uintptr_t)((char *)ptr - (char *)mp->pool);
    uint32_t idx = (uint32_t)(offset / mp->object_size);

    if (idx >= mp->capacity) {
        return; /* Invalid pointer — not from this pool */
    }

    /* Write the index FIRST (before publishing the free_top increment).
     * This ensures any consumer that sees the new free_top will also
     * see the written index. The __ATOMIC_RELEASE on free_top guarantees
     * this write is visible before the increment. */
    uint32_t slot = __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
    if (slot >= mp->capacity) {
        return; /* Double-free detected */
    }
    mp->free_list[slot] = idx;

    /* Publish: release store ensures free_list[slot] is visible first */
    __atomic_store_n(&mp->free_top, slot + 1, __ATOMIC_RELEASE);
}

uint32_t mempool_available(const mempool_t *mp) {
    if (!mp) return 0;
    return __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
}

uint32_t mempool_capacity(const mempool_t *mp) {
    return mp ? mp->capacity : 0;
}
