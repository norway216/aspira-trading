/**
 * feed_handler.h — Market Data Feed Handler
 *
 * Low-level C component for receiving and normalizing market data.
 * Uses epoll for efficient I/O multiplexing.
 * Supports:
 *   - UDP multicast reception
 *   - Binary protocol decoding
 *   - Message normalization
 *   - Push to lock-free output queue
 *   - Simulated feed for testing
 */
#ifndef FEED_HANDLER_H
#define FEED_HANDLER_H

#include "ring_buffer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum symbol length */
#define FEED_MAX_SYMBOL 16

/* Maximum message size (binary wire format) */
#define FEED_MAX_MSG_SIZE 256

/**
 * A normalized market data message from the feed.
 */
typedef struct {
    uint64_t timestamp_ns;    /* Exchange timestamp or receive time */
    double   bid_price;
    double   ask_price;
    int32_t  bid_size;
    int32_t  ask_size;
    double   last_price;
    int32_t  last_size;
    int32_t  volume;
    char     symbol[FEED_MAX_SYMBOL];
    char     _pad[4];
} feed_msg_t;

/**
 * Feed handler configuration.
 */
typedef struct {
    char     listen_addr[64];  /* IP to bind (e.g. "0.0.0.0") */
    uint16_t listen_port;      /* UDP port */
    bool     simulation_mode;  /* Use simulated data instead of real socket */
    uint32_t sim_interval_us;  /* Microseconds between simulated messages */
    uint32_t output_queue_size; /* Ring buffer capacity for output */
} feed_config_t;

/**
 * Feed handler opaque handle.
 */
typedef struct feed_handler_t feed_handler_t;

/**
 * Create and initialize the feed handler.
 * @param config  Configuration (copied internally)
 * @return Handle, or NULL on error
 */
feed_handler_t *feed_init(const feed_config_t *config);

/**
 * Start the feed handler (starts the receive thread).
 * @return 0 on success, -1 on error
 */
int feed_start(feed_handler_t *fh);

/**
 * Stop the feed handler and join the receive thread.
 */
void feed_stop(feed_handler_t *fh);

/**
 * Destroy the feed handler and free all resources.
 */
void feed_destroy(feed_handler_t *fh);

/**
 * Get the output ring buffer (consumer interface).
 * Pop messages from this buffer to receive normalized market data.
 */
ring_buffer_t *feed_get_output_queue(feed_handler_t *fh);

/**
 * Return a consumed message to the feed handler's memory pool.
 * Must be called by the consumer after processing a message from the output queue.
 * This replaces free() — the message is recycled, not deallocated.
 */
void feed_return_msg(feed_handler_t *fh, feed_msg_t *msg);

/**
 * Get statistics about the feed handler.
 */
void feed_get_stats(feed_handler_t *fh, uint64_t *msgs_received,
                    uint64_t *msgs_dropped, uint64_t *bytes_received);

#ifdef __cplusplus
}
#endif

#endif /* FEED_HANDLER_H */
