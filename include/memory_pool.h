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
    void *pool;            /* contiguous block of objects */
    uint32_t object_size;  /* size of each object */
    uint32_t capacity;     /* total number of objects */
    /* free_top is on its own cache line */
    uint32_t free_top;     /* top of free stack (atomic via __atomic builtins) */
    char _pad[MEMPOOL_CACHE_LINE - sizeof(uint32_t)];
    uint32_t *free_list;   /* array of indices (the stack) */
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
 * Allocate one object from the pool (O(1)).
 * Returns NULL if the pool is exhausted.
 * Thread-safe — can be called from multiple threads.
 */
void *mempool_alloc(mempool_t *mp);

/**
 * Return an object to the pool (O(1)).
 * Thread-safe.
 */
void mempool_free(mempool_t *mp, void *ptr);

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
