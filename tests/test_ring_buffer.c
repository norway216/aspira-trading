/**
 * test_ring_buffer.c — Unit tests for lock-free SPSC ring buffer
 *
 * NOTE: We NEVER use assert() with function calls that have side effects.
 * assert() may be compiled away by -DNDEBUG.
 */
#include "../include/ring_buffer.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CAPACITY 64

static int test_basic_push_pop() {
    ring_buffer_t rb;
    int ret = ring_buffer_init(&rb, TEST_CAPACITY);
    assert(ret == 0);
    assert(ring_buffer_empty(&rb) == true);
    assert(ring_buffer_full(&rb) == false);

    int values[10];
    for (int i = 0; i < 10; i++) {
        values[i] = i + 100;
        bool pushed = ring_buffer_push(&rb, &values[i]);
        assert(pushed == true);
    }

    assert(ring_buffer_count(&rb) == 10);
    assert(ring_buffer_empty(&rb) == false);

    for (int i = 0; i < 10; i++) {
        int *v = (int *)ring_buffer_pop(&rb);
        assert(v != NULL);
        assert(*v == values[i]);
    }

    assert(ring_buffer_empty(&rb) == true);
    void *null_pop = ring_buffer_pop(&rb);
    assert(null_pop == NULL);

    ring_buffer_destroy(&rb);
    printf("  [PASS] test_basic_push_pop\n");
    return 0;
}

static int test_full_buffer() {
    ring_buffer_t rb;
    int ret = ring_buffer_init(&rb, 4);
    assert(ret == 0); /* 4 slots, 3 usable */

    int v1 = 1, v2 = 2, v3 = 3, v4 = 4;
    assert(ring_buffer_push(&rb, &v1) == true);
    assert(ring_buffer_push(&rb, &v2) == true);
    assert(ring_buffer_push(&rb, &v3) == true);
    assert(ring_buffer_push(&rb, &v4) == false); /* Should be full */

    assert(ring_buffer_full(&rb) == true);

    void *popped = ring_buffer_pop(&rb);
    assert(popped == &v1);
    assert(ring_buffer_push(&rb, &v4) == true); /* Now has room */

    ring_buffer_destroy(&rb);
    printf("  [PASS] test_full_buffer\n");
    return 0;
}

static int test_wrap_around() {
    ring_buffer_t rb;
    int ret = ring_buffer_init(&rb, 8);
    assert(ret == 0);

    int values[20];
    for (int i = 0; i < 20; i++) values[i] = i;

    /* Push 7, pop 7 — wrap the indices */
    for (int iter = 0; iter < 10; iter++) {
        for (int i = 0; i < 7; i++) {
            assert(ring_buffer_push(&rb, &values[i]) == true);
        }
        for (int i = 0; i < 7; i++) {
            int *v = (int *)ring_buffer_pop(&rb);
            assert(v != NULL);
            assert(*v == values[i]);
        }
    }

    assert(ring_buffer_empty(&rb) == true);

    ring_buffer_destroy(&rb);
    printf("  [PASS] test_wrap_around\n");
    return 0;
}

typedef struct {
    ring_buffer_t *rb;
    int *items;
    int count;
} thread_arg_t;

static void *producer_thread(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = 0; i < ta->count; i++) {
        while (!ring_buffer_push(ta->rb, &ta->items[i])) {
            /* busy-wait */
        }
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int received = 0;
    while (received < ta->count) {
        int *v = (int *)ring_buffer_pop(ta->rb);
        if (v) {
            received++;
        }
    }
    return NULL;
}

static int test_threaded() {
    ring_buffer_t rb;
    int ret = ring_buffer_init(&rb, 1024);
    assert(ret == 0);

    const int N = 100000;
    int *items = (int *)malloc(N * sizeof(int));
    assert(items != NULL);
    for (int i = 0; i < N; i++) items[i] = i;

    thread_arg_t prod_arg = {&rb, items, N};
    thread_arg_t cons_arg = {&rb, items, N};

    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_thread, &prod_arg);
    pthread_create(&cons, NULL, consumer_thread, &cons_arg);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    assert(ring_buffer_empty(&rb) == true);
    free(items);
    ring_buffer_destroy(&rb);

    printf("  [PASS] test_threaded (%d items)\n", N);
    return 0;
}

int main() {
    printf("Ring Buffer Tests:\n");
    test_basic_push_pop();
    test_full_buffer();
    test_wrap_around();
    test_threaded();
    printf("All ring buffer tests passed.\n");
    return 0;
}
