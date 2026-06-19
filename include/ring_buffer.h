/**
 * ring_buffer.h — Lock-free SPSC (Single Producer, Single Consumer) Ring Buffer
 *
 * Cache-line padded to prevent false sharing.
 * Uses GCC __atomic builtins for cross-language (C/C++) compatibility.
 * Power-of-2 capacity ensures fast modulo via bitmask.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cache line size (x86-64 typical) */
#define CACHE_LINE_SIZE 64

/* Default ring buffer capacities (must be power of 2) */
#define RING_BUFFER_CAPACITY 1024

/**
 * Lock-free SPSC ring buffer.
 * Uses GCC __atomic builtins for C/C++ portability.
 * Cache-line padded to prevent false sharing.
 */
typedef struct {
    /* Producer writes to `head` */
    uint_fast32_t head;
    /* Padding — put head on its own cache line */
    char _pad1[CACHE_LINE_SIZE - sizeof(uint_fast32_t)];

    /* Consumer reads from `tail` */
    uint_fast32_t tail;
    /* Padding */
    char _pad2[CACHE_LINE_SIZE - sizeof(uint_fast32_t)];

    /* Buffer array */
    void **buffer;
    uint32_t capacity;  /* total slots, power of 2 */
    uint32_t mask;      /* capacity - 1, for fast modulo */
} ring_buffer_t;

/**
 * Initialize a ring buffer.
 * @param rb   Pointer to uninitialized ring_buffer_t
 * @param capacity  Must be a power of 2 (e.g. 1024, 2048)
 * @return 0 on success, -1 on error
 */
int ring_buffer_init(ring_buffer_t *rb, uint32_t capacity);

/**
 * Destroy a ring buffer and free its internal storage.
 */
void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * Producer-side: push one pointer into the buffer (static inline — hot path).
 * Returns false if full (non-blocking).
 * NOT thread-safe — only ONE producer thread.
 */
static inline bool ring_buffer_push(ring_buffer_t *rb, void *data) {
    /* Load head (producer-owned) */
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    /* Load tail — RELAXED sufficient: only need full/not-full check */
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

/**
 * Consumer-side: pop one pointer from the buffer (static inline — hot path).
 * Returns NULL if empty (non-blocking).
 * NOT thread-safe — only ONE consumer thread.
 */
static inline void *ring_buffer_pop(ring_buffer_t *rb) {
    /* Load tail (consumer-owned) */
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    /* Load head — acquire to see producer's writes */
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        return NULL; /* Empty */
    }

    void *data = rb->buffer[tail];

    /* Publish — release ensures producer sees the freed slot */
    __atomic_store_n(&rb->tail, (tail + 1) & rb->mask, __ATOMIC_RELEASE);
    return data;
}

/**
 * Return the number of elements currently in the buffer (snapshot).
 */
uint32_t ring_buffer_count(const ring_buffer_t *rb);

/**
 * Return true if the buffer is empty.
 */
bool ring_buffer_empty(const ring_buffer_t *rb);

/**
 * Return true if the buffer is full.
 */
bool ring_buffer_full(const ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
