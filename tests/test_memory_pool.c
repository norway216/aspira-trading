/**
 * test_memory_pool.c — Unit tests for memory pool
 *
 * NOTE: We NEVER use assert() with function calls that have side effects.
 */
#include "../include/memory_pool.h"
#include <assert.h>
#include <pthread.h>
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

/* ---- Multi-threaded stress test ---- */

#define MT_POOL_SIZE    1024
#define MT_ITERATIONS   10000
#define MT_NUM_THREADS  4

typedef struct {
    mempool_t *pool;
    int iterations;
    int thread_id;
    uint32_t prng_state; /* Per-thread xorshift32 PRNG (avoids rand() lock contention) */
} mt_thread_arg_t;

/* Per-thread xorshift32 PRNG — thread-safe, no lock contention */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t rand_mod(uint32_t *state, uint32_t mod) {
    return xorshift32(state) % mod;
}

static void *mt_worker(void *arg) {
    mt_thread_arg_t *ta = (mt_thread_arg_t *)arg;
    void *ptrs[16];  /* Each thread holds at most 16 objects at a time */
    int held = 0;

    for (int i = 0; i < ta->iterations; i++) {
        if (held < 4 || rand_mod(&ta->prng_state, 3) == 0) {
            /* Allocate */
            if (held < 16) {
                void *p = mempool_alloc(ta->pool);
                if (p) {
                    /* Mark with thread ID + iteration */
                    *(uint32_t *)p = (uint32_t)((ta->thread_id << 24) | (i & 0xFFFFFF));
                    ptrs[held++] = p;
                }
            }
            /* If held >= 16, skip allocation (nowhere to store the pointer) */
        } else {
            /* Free one random held object */
            int idx = (int)rand_mod(&ta->prng_state, (uint32_t)held);
            mempool_free(ta->pool, ptrs[idx]);
            ptrs[idx] = ptrs[held - 1];
            held--;
        }
    }

    /* Free remaining held objects */
    for (int i = 0; i < held; i++) {
        mempool_free(ta->pool, ptrs[i]);
    }

    return NULL;
}

static int test_multi_threaded() {
    mempool_t mp;
    int ret = mempool_init(&mp, sizeof(uint32_t), MT_POOL_SIZE);
    assert(ret == 0);

    pthread_t threads[MT_NUM_THREADS];
    mt_thread_arg_t args[MT_NUM_THREADS];

    for (int i = 0; i < MT_NUM_THREADS; i++) {
        args[i].pool = &mp;
        args[i].iterations = MT_ITERATIONS;
        args[i].thread_id = i;
        args[i].prng_state = 2463534242U + (uint32_t)(i * 7919); /* Distinct per-thread seed */
        pthread_create(&threads[i], NULL, mt_worker, &args[i]);
    }

    for (int i = 0; i < MT_NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* After all threads finish and return their objects, the pool should be
     * back to full capacity (no leaks, no corruption). */
    uint32_t avail = mempool_available(&mp);
    assert(avail == MT_POOL_SIZE);

    /* Verify we can still alloc and free */
    for (int i = 0; i < 100; i++) {
        void *p = mempool_alloc(&mp);
        assert(p != NULL);
        mempool_free(&mp, p);
    }

    assert(mempool_available(&mp) == MT_POOL_SIZE);

    mempool_destroy(&mp);
    printf("  [PASS] test_multi_threaded (%d threads × %d iterations)\n",
           MT_NUM_THREADS, MT_ITERATIONS);
    return 0;
}

int main() {
    printf("Memory Pool Tests:\n");
    test_alloc_free();
    test_pointer_validity();
    test_multi_threaded();
    printf("All memory pool tests passed.\n");
    return 0;
}
