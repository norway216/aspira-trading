/**
 * rate_limiter.h — Token Bucket Rate Limiter
 *
 * Implements a lock-free token bucket algorithm for order rate limiting.
 *
 * Design (from hft_risk_engine_design.md §9):
 *   - Atomic token counter — lock-free allow() in hot path
 *   - Configurable refill rate (tokens/sec) and burst capacity
 *   - Deterministic execution path
 *   - Zero heap allocation
 *
 * The token bucket is more sophisticated than a simple OPS counter:
 * it allows short bursts up to `burst_capacity` while enforcing the
 * long-term rate `refill_rate`. This prevents unnecessary rejection
 * of legitimate order bursts while still protecting against runaway.
 *
 * Thread safety:
 *   - allow() is safe for multiple concurrent callers
 *   - refill() is called on every allow() — uses monotonic clock
 */
#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <cstdint>
#include <ctime>

/* We want to keep this header C-compatible where possible */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/**
 * Lock-free Token Bucket Rate Limiter.
 *
 * alignas(64) to prevent false sharing with adjacent data in the
 * RiskEngine or any parent structure.
 */
struct alignas(64) TokenBucket {
    int32_t  tokens;            /* Current token count (atomic via __atomic builtins) */
    int32_t  burst_capacity;    /* Max tokens that can accumulate */
    int32_t  refill_rate;       /* Tokens refilled per second */
    uint64_t last_refill_ns;    /* Timestamp of last refill (monotonic) */
    int32_t  _pad[4];           /* Pad to 64 bytes total */
};

/**
 * Initialize a token bucket.
 *
 * @param tb              Pointer to uninitialized TokenBucket
 * @param refill_rate     Tokens per second (sustained rate)
 * @param burst_capacity  Max burst size (0 = same as refill_rate)
 */
static inline void tb_init(TokenBucket *tb, int32_t refill_rate, int32_t burst_capacity) {
    if (burst_capacity <= 0) burst_capacity = refill_rate;
    if (burst_capacity < refill_rate) burst_capacity = refill_rate;

    tb->tokens = burst_capacity;
    tb->burst_capacity = burst_capacity;
    tb->refill_rate = refill_rate;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tb->last_refill_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Refill tokens based on elapsed time.
 * Called internally by tb_allow() — not needed externally.
 */
static inline void tb_refill(TokenBucket *tb) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    uint64_t elapsed_ns = now_ns - tb->last_refill_ns;
    if (elapsed_ns == 0) return;

    /* Calculate new tokens: elapsed_seconds * refill_rate, truncated to int */
    int32_t new_tokens = (int32_t)((elapsed_ns * (uint64_t)tb->refill_rate) / 1000000000ULL);
    if (new_tokens > 0) {
        tb->last_refill_ns = now_ns;

        /* Atomically add tokens, capping at burst_capacity */
        int32_t old = __atomic_load_n(&tb->tokens, __ATOMIC_RELAXED);
        int32_t desired;
        do {
            desired = old + new_tokens;
            if (desired > tb->burst_capacity) desired = tb->burst_capacity;
        } while (!__atomic_compare_exchange_n(&tb->tokens, &old, desired,
                                                false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }
}

/**
 * Check if an order is allowed under the rate limit.
 * Consumes 1 token if allowed.
 *
 * Hot-path function — lock-free, no syscalls (vDSO clock_gettime only).
 *
 * @param tb  Token bucket
 * @return true if allowed, false if rate limit exceeded
 */
static inline bool tb_allow(TokenBucket *tb) {
    tb_refill(tb);

    /* Try to consume 1 token atomically */
    int32_t old = __atomic_load_n(&tb->tokens, __ATOMIC_RELAXED);
    int32_t desired;
    do {
        if (old <= 0) return false;  /* No tokens available */
        desired = old - 1;
    } while (!__atomic_compare_exchange_n(&tb->tokens, &old, desired,
                                            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return true;
}

/**
 * Get current token count (snapshot — may be stale).
 */
static inline int32_t tb_tokens(const TokenBucket *tb) {
    return __atomic_load_n(&tb->tokens, __ATOMIC_RELAXED);
}

/**
 * Reset the token bucket to full capacity.
 */
static inline void tb_reset(TokenBucket *tb) {
    __atomic_store_n(&tb->tokens, tb->burst_capacity, __ATOMIC_RELAXED);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tb->last_refill_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Dynamically update refill rate and burst capacity.
 */
static inline void tb_update(TokenBucket *tb, int32_t refill_rate, int32_t burst_capacity) {
    if (burst_capacity <= 0) burst_capacity = refill_rate;
    if (burst_capacity < refill_rate) burst_capacity = refill_rate;
    tb->refill_rate = refill_rate;
    tb->burst_capacity = burst_capacity;

    /* Clamp current tokens to new capacity */
    int32_t old = __atomic_load_n(&tb->tokens, __ATOMIC_RELAXED);
    int32_t desired;
    do {
        desired = old;
        if (desired > burst_capacity) desired = burst_capacity;
    } while (!__atomic_compare_exchange_n(&tb->tokens, &old, desired,
                                            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

#endif /* __cplusplus */

#endif /* RATE_LIMITER_H */
