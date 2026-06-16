/**
 * execution_gateway.h — Execution Gateway
 *
 * Manages outbound order flow to exchanges/brokers.
 * Features:
 *   - TCP connection management (connect/reconnect)
 *   - Order serialization and sending
 *   - Acknowledgement tracking
 *   - Retry with exponential backoff
 *
 * In this implementation, orders are serialized to a simple text protocol
 * and can be sent over TCP or logged to a file for simulation.
 */
#ifndef EXECUTION_GATEWAY_H
#define EXECUTION_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum order message size */
#define GW_MAX_MSG_SIZE 512

/* Connection states */
typedef enum {
    GW_DISCONNECTED = 0,
    GW_CONNECTING   = 1,
    GW_CONNECTED    = 2,
    GW_ERROR        = 3
} gw_conn_state_t;

/**
 * A single pending acknowledgement.
 */
typedef struct {
    int32_t  order_id;        /* Order waiting for ack */
    uint64_t send_time_ns;    /* When the order was sent */
    uint32_t retry_count;     /* Number of retries */
    bool     acked;           /* Acknowledgement received */
} gw_ack_entry_t;

/**
 * Execution gateway handle.
 */
typedef struct execution_gateway_t execution_gateway_t;

/**
 * Configuration for the execution gateway.
 */
typedef struct {
    char     host[64];          /* Remote host (empty = simulation mode) */
    uint16_t port;             /* Remote port */
    uint32_t max_retries;      /* Max retries before giving up */
    uint64_t retry_delay_ns;   /* Base delay between retries (nanoseconds) */
    uint64_t connect_timeout_ns; /* Connection timeout */
    uint32_t max_pending_acks; /* Max unacknowledged orders */
    bool     simulation_mode;  /* If true, log to file instead of sending */
    char     sim_log_file[256]; /* Log file for simulation mode */
} gw_config_t;

/**
 * Callback invoked when an acknowledgement is received.
 * @param order_id   The acknowledged order ID
 * @param status     0 = accepted, non-zero = rejected
 * @param user_data  Opaque user pointer
 */
typedef void (*gw_ack_callback_t)(int32_t order_id, int status, void *user_data);

/**
 * Create and initialize the execution gateway.
 */
execution_gateway_t *gw_init(const gw_config_t *config);

/**
 * Shutdown the gateway gracefully (close connections, flush).
 */
void gw_shutdown(execution_gateway_t *gw);

/**
 * Destroy and free the gateway.
 */
void gw_destroy(execution_gateway_t *gw);

/**
 * Send an order to the exchange.
 * @param gw      Gateway handle
 * @param msg     Serialized order message
 * @param msg_len Message length
 * @param order_id Order ID for ack tracking
 * @return 0 on success, -1 on error
 */
int gw_send_order(execution_gateway_t *gw, const char *msg,
                  uint32_t msg_len, int32_t order_id);

/**
 * Register a callback for order acknowledgements.
 */
void gw_set_ack_callback(execution_gateway_t *gw, gw_ack_callback_t cb,
                         void *user_data);

/**
 * Get current connection state.
 */
gw_conn_state_t gw_get_state(const execution_gateway_t *gw);

/**
 * Get the number of pending (unacknowledged) orders.
 */
uint32_t gw_pending_acks(const execution_gateway_t *gw);

/**
 * Serialize an order to the gateway protocol format.
 * Format: "ORDER|<id>|<symbol>|<side>|<type>|<price>|<qty>|<timestamp>"
 */
int gw_serialize_order(char *buf, uint32_t buf_size,
                       int32_t order_id, const char *symbol,
                       char side, char order_type,
                       double price, int32_t quantity, uint64_t timestamp_ns);

#ifdef __cplusplus
}
#endif

#endif /* EXECUTION_GATEWAY_H */
