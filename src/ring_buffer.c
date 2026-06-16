/**
 * ring_buffer.c — Lock-free SPSC Ring Buffer Implementation
 *
 * Uses GCC __atomic builtins for C/C++ cross-language compatibility.
 * __ATOMIC_RELEASE / __ATOMIC_ACQUIRE provide the necessary memory ordering.
 */
#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>

/* Check if a number is a power of 2 */
static bool is_power_of_two(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

/* Round up to the next power of 2 */
static uint32_t next_power_of_two(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

int ring_buffer_init(ring_buffer_t *rb, uint32_t capacity) {
    if (!rb || capacity == 0) return -1;

    /* Ensure power-of-2 capacity */
    if (!is_power_of_two(capacity)) {
        capacity = next_power_of_two(capacity);
    }

    rb->buffer = (void **)calloc(capacity, sizeof(void *));
    if (!rb->buffer) return -1;

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->head = 0;
    rb->tail = 0;

    return 0;
}

void ring_buffer_destroy(ring_buffer_t *rb) {
    if (!rb) return;
    free(rb->buffer);
    rb->buffer = NULL;
    rb->capacity = 0;
    rb->mask = 0;
}

bool ring_buffer_push(ring_buffer_t *rb, void *data) {
    if (!rb || !data) return false;

    /* Load head (producer-owned) */
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    /* Load tail (consumer-owned). RELAXED is sufficient: producer only needs
     * to know if the buffer is full; the actual slot ownership is guarded
     * by the head release/acquire pair in push/pop respectively. */
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);

    uint32_t next = (head + 1) & rb->mask;
    if (next == tail) {
        return false; /* Full */
    }

    rb->buffer[head] = data;

    /* Publish — release ensures consumer sees the stored pointer */
    __atomic_store_n(&rb->head, next, __ATOMIC_RELEASE);
    return true;
}

void *ring_buffer_pop(ring_buffer_t *rb) {
    if (!rb) return NULL;

    /* Load tail (consumer-owned) */
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    /* Load head (producer-owned) — acquire to see producer's writes */
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        return NULL; /* Empty */
    }

    void *data = rb->buffer[tail];

    /* Publish — release ensures producer sees the freed slot */
    __atomic_store_n(&rb->tail, (tail + 1) & rb->mask, __ATOMIC_RELEASE);
    return data;
}

uint32_t ring_buffer_count(const ring_buffer_t *rb) {
    if (!rb) return 0;
    /* Snapshot — RELAXED is sufficient; inherently racy */
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    /* Branch-free: unsigned wraparound handled by mask */
    return (head - tail) & rb->mask;
}

bool ring_buffer_empty(const ring_buffer_t *rb) {
    if (!rb) return true;
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    return head == tail;
}

bool ring_buffer_full(const ring_buffer_t *rb) {
    if (!rb) return false;
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    return ((head + 1) & rb->mask) == tail;
}
