/**
 * test_memory_pool.c — Unit tests for memory pool
 *
 * NOTE: We NEVER use assert() with function calls that have side effects.
 */
#include "../include/memory_pool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_alloc_free() {
    mempool_t mp;
    int ret = mempool_init(&mp, 64, 16);
    assert(ret == 0);
    assert(mempool_available(&mp) == 16);
    assert(mempool_capacity(&mp) == 16);

    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        ptrs[i] = mempool_alloc(&mp);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xAA, 64);
    }

    assert(mempool_available(&mp) == 0);
    void *null_alloc = mempool_alloc(&mp);
    assert(null_alloc == NULL); /* Pool exhausted */

    /* Free half */
    for (int i = 0; i < 8; i++) {
        mempool_free(&mp, ptrs[i]);
    }
    assert(mempool_available(&mp) == 8);

    /* Re-allocate */
    for (int i = 0; i < 8; i++) {
        void *p = mempool_alloc(&mp);
        assert(p != NULL);
    }

    assert(mempool_available(&mp) == 0);

    mempool_destroy(&mp);
    printf("  [PASS] test_alloc_free\n");
    return 0;
}

static int test_pointer_validity() {
    mempool_t mp;
    int ret = mempool_init(&mp, 128, 32);
    assert(ret == 0);

    void *p1 = mempool_alloc(&mp);
    void *p2 = mempool_alloc(&mp);
    assert(p1 != NULL);
    assert(p2 != NULL);

    /* Verify pointers are within the pool */
    uintptr_t base = (uintptr_t)mp.pool;
    assert((uintptr_t)p1 >= base);
    assert((uintptr_t)p2 >= base);
    assert(p1 != p2);

    /* Write to the memory and verify it persists */
    strcpy((char *)p1, "Hello, Pool!");
    mempool_free(&mp, p1);

    void *p3 = mempool_alloc(&mp);
    assert(p3 != NULL);
    strcpy((char *)p3, "Reused slot");
    assert(strcmp((char *)p3, "Reused slot") == 0);

    mempool_destroy(&mp);
    printf("  [PASS] test_pointer_validity\n");
    return 0;
}

int main() {
    printf("Memory Pool Tests:\n");
    test_alloc_free();
    test_pointer_validity();
    printf("All memory pool tests passed.\n");
    return 0;
}
