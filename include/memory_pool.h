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
    /* Atomically decrement free_top. RELAXED sufficient: we only need
     * atomicity of the counter; the free_list reads are guarded by the
     * RELEASE store in mempool_free. */
    uint32_t old_top = __atomic_fetch_sub(&mp->free_top, 1, __ATOMIC_RELAXED);
    if (old_top == 0) {
        __atomic_fetch_add(&mp->free_top, 1, __ATOMIC_RELAXED);
        return NULL; /* Pool exhausted */
    }
    uint32_t idx = mp->free_list[old_top - 1];
    return (char *)mp->pool + (size_t)idx * mp->object_size;
}

/**
 * Return an object to the pool — O(1), static inline (hot path).
 * Writes index BEFORE the RELEASE store on free_top for correct visibility.
 * Safe for single-consumer free; for multi-consumer, a CAS is needed.
 */
static inline void mempool_free(mempool_t *mp, void *ptr) {
    uintptr_t offset = (uintptr_t)((char *)ptr - (char *)mp->pool);
    uint32_t idx = (uint32_t)(offset / mp->object_size);
    if (idx >= mp->capacity) return; /* Not from this pool */

    /* Read free_top, write index, then publish with RELEASE */
    uint32_t slot = __atomic_load_n(&mp->free_top, __ATOMIC_RELAXED);
    if (slot >= mp->capacity) return; /* Double-free guard */
    mp->free_list[slot] = idx;
    __atomic_store_n(&mp->free_top, slot + 1, __ATOMIC_RELEASE);
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
