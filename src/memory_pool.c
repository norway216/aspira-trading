/**
 * memory_pool.c — Pre-allocated Memory Pool Implementation
 *
 * Thread-safe via GCC legacy __sync builtins for broadest compatibility.
 * These provide full memory barriers and work on any aligned integer.
 */
#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>

int mempool_init(mempool_t *mp, size_t object_size, uint32_t capacity) {
    if (!mp || object_size == 0 || capacity == 0) return -1;

    /* Ensure object can hold an index for the free list */
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

    /* Initialize free list: stack-like, top points to next available index */
    for (uint32_t i = 0; i < capacity; i++) {
        mp->free_list[i] = capacity - 1 - i; /* [cap-1, cap-2, ..., 0] */
    }
    mp->free_top = capacity; /* Points past the last element = full stack */

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

    /* Atomically decrement free_top, returning the old value.
     * If old value is 0, pool is exhausted. */
    uint32_t old_top = __sync_fetch_and_sub(&mp->free_top, 1);
    if (old_top == 0) {
        /* Pool was exhausted — undo the decrement */
        __sync_fetch_and_add(&mp->free_top, 1);
        return NULL;
    }

    /* old_top was capacity, capacity-1, ..., or 1.
     * After decrement, free_top = old_top - 1.
     * The index we return is at free_list[old_top - 1]. */
    uint32_t idx = mp->free_list[old_top - 1];
    return (char *)mp->pool + (size_t)idx * mp->object_size;
}

void mempool_free(mempool_t *mp, void *ptr) {
    if (!mp || !ptr) return;

    /* Calculate index from pointer offset */
    uintptr_t offset = (uintptr_t)((char *)ptr - (char *)mp->pool);
    uint32_t idx = (uint32_t)(offset / mp->object_size);

    if (idx >= mp->capacity) {
        return; /* Invalid pointer — not from this pool */
    }

    /* Atomically increment free_top, getting the OLD value.
     * We store our index at free_list[old_val]. */
    uint32_t slot = __sync_fetch_and_add(&mp->free_top, 1);
    if (slot >= mp->capacity) {
        /* Pool is fully free — double-free detected, roll back */
        __sync_fetch_and_sub(&mp->free_top, 1);
        return;
    }
    mp->free_list[slot] = idx;
}

uint32_t mempool_available(const mempool_t *mp) {
    if (!mp) return 0;
    /* Snapshot read — __sync_fetch_and_add with 0 is a full-barrier read */
    return __sync_fetch_and_add((uint32_t *)&mp->free_top, 0);
}

uint32_t mempool_capacity(const mempool_t *mp) {
    return mp ? mp->capacity : 0;
}
