/**
 * execution_gateway.c — Execution Gateway Implementation
 */
#include "execution_gateway.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct execution_gateway_t {
    gw_config_t config;
    gw_conn_state_t state;
    FILE *sim_file;            /* Simulation mode: log file */
    gw_ack_callback_t ack_cb;
    void *ack_user_data;
    uint64_t orders_sent;
    uint64_t orders_acked;
    uint64_t orders_rejected;
};

/* ---- Helpers ---- */

static uint64_t now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- Public API ---- */

execution_gateway_t *gw_init(const gw_config_t *config) {
    if (!config) return NULL;

    execution_gateway_t *gw = (execution_gateway_t *)calloc(1, sizeof(execution_gateway_t));
    if (!gw) return NULL;

    gw->config = *config;

    if (config->simulation_mode) {
        gw->sim_file = fopen(config->sim_log_file, "a");
        if (gw->sim_file) {
            fprintf(gw->sim_file, "=== Execution Gateway Started at %lu ===\n",
                    (unsigned long)time(NULL));
            fflush(gw->sim_file);
        }
        gw->state = GW_CONNECTED; /* Simulated connection */
    } else if (config->host[0] != '\0') {
        gw->state = GW_DISCONNECTED;
        /* In a real implementation, we would connect() here.
         * For this reference implementation, we simulate the connection. */
        gw->state = GW_CONNECTED;
    } else {
        gw->state = GW_DISCONNECTED;
    }

    return gw;
}

void gw_shutdown(execution_gateway_t *gw) {
    if (!gw) return;

    if (gw->sim_file) {
        fprintf(gw->sim_file, "=== Execution Gateway Shutdown ===\n");
        fprintf(gw->sim_file, "  Orders sent:     %lu\n",
                (unsigned long)gw->orders_sent);
        fprintf(gw->sim_file, "  Orders acked:    %lu\n",
                (unsigned long)gw->orders_acked);
        fprintf(gw->sim_file, "  Orders rejected: %lu\n",
                (unsigned long)gw->orders_rejected);
        fflush(gw->sim_file);
    }

    gw->state = GW_DISCONNECTED;
}

void gw_destroy(execution_gateway_t *gw) {
    if (!gw) return;
    if (gw->sim_file) {
        fclose(gw->sim_file);
        gw->sim_file = NULL;
    }
    free(gw);
}

int gw_send_order(execution_gateway_t *gw, const char *msg,
                  uint32_t msg_len, int32_t order_id) {
    if (!gw || !msg || msg_len == 0) return -1;

    if (gw->state != GW_CONNECTED) {
        return -1;
    }

    uint64_t send_time = now_nanos();

    if (gw->sim_file) {
        /* Simulation mode: log to file */
        fprintf(gw->sim_file, "SEND | %lu | %s\n",
                (unsigned long)send_time, msg);
        fflush(gw->sim_file);

        /* Auto-acknowledge in simulation mode */
        gw->orders_acked++;
        if (gw->ack_cb) {
            gw->ack_cb(order_id, 0, gw->ack_user_data); /* 0 = accepted */
        }
    } else {
        /* Real mode: send over TCP socket (not implemented in reference) */
        /* In production, this would do a non-blocking send() on the socket */
    }

    gw->orders_sent++;
    return 0;
}

void gw_set_ack_callback(execution_gateway_t *gw, gw_ack_callback_t cb,
                         void *user_data) {
    if (!gw) return;
    gw->ack_cb = cb;
    gw->ack_user_data = user_data;
}

gw_conn_state_t gw_get_state(const execution_gateway_t *gw) {
    return gw ? gw->state : GW_ERROR;
}

uint32_t gw_pending_acks(const execution_gateway_t *gw) {
    if (!gw) return 0;
    return (uint32_t)(gw->orders_sent - gw->orders_acked);
}

int gw_serialize_order(char *buf, uint32_t buf_size,
                       int32_t order_id, const char *symbol,
                       char side, char order_type,
                       double price, int32_t quantity, uint64_t timestamp_ns) {
    if (!buf || buf_size == 0) return -1;

    const char *side_str = (side == 'B') ? "BUY" : "SELL";
    const char *type_str = (order_type == 'L') ? "LIMIT" : "MARKET";

    int written = snprintf(buf, buf_size,
                           "ORDER|%d|%s|%s|%s|%.4f|%d|%lu",
                           order_id, symbol, side_str, type_str,
                           price, quantity, (unsigned long)timestamp_ns);

    return (written > 0 && (uint32_t)written < buf_size) ? 0 : -1;
}
