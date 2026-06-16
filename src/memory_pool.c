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

uint32_t mempool_available(const mempool_t *mp) {
    if (!mp) return 0;
    return __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
}

uint32_t mempool_capacity(const mempool_t *mp) {
    return mp ? mp->capacity : 0;
}
