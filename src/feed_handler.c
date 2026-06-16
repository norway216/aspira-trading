/**
 * feed_handler.c — Market Data Feed Handler Implementation
 *
 * In simulation mode, generates realistic market data.
 * In production mode, uses epoll + UDP socket.
 */
#include "feed_handler.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* For socket/network in production mode */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

struct feed_handler_t {
    feed_config_t config;
    ring_buffer_t output_queue;

    /* Thread */
    pthread_t recv_thread;
    volatile bool running;

    /* Sockets (production mode) */
    int sockfd;

    /* Stats */
    volatile uint64_t msgs_received;
    volatile uint64_t msgs_dropped;
    volatile uint64_t bytes_received;

    /* Simulation state */
    double sim_base_price;
    double sim_base_bid;
    double sim_base_ask;
};

/* ---- Helpers ---- */

static uint64_t now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_us(uint32_t us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

/* Simple pseudo-random number generator (xorshift32).
 * We avoid rand() to prevent lock contention on the global PRNG state. */
static uint32_t xorshift32_state = 2463534242U;

static uint32_t xorshift32(void) {
    uint32_t x = xorshift32_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift32_state = x;
    return x;
}

/* Generate a random double in [0, 1) */
static double rand_double(void) {
    return (double)(xorshift32() & 0xFFFFFF) / (double)0x1000000;
}

/* Generate a simulated market data message */
static void simulate_message(feed_handler_t *fh, feed_msg_t *msg) {
    /* Random walk for prices */
    double noise = (rand_double() - 0.5) * 0.02;
    fh->sim_base_price += noise;

    /* Keep price in a reasonable range */
    if (fh->sim_base_price < 10.0) fh->sim_base_price = 10.0;
    if (fh->sim_base_price > 1000.0) fh->sim_base_price = 1000.0;

    double spread_pct = 0.0001 + rand_double() * 0.0005; /* 1-5 bps */

    msg->bid_price = fh->sim_base_price * (1.0 - spread_pct / 2.0);
    msg->ask_price = fh->sim_base_price * (1.0 + spread_pct / 2.0);
    msg->bid_size = (int32_t)(100 + rand_double() * 9900);  /* 100 - 10000 */
    msg->ask_size = (int32_t)(100 + rand_double() * 9900);
    msg->last_price = fh->sim_base_price;
    msg->last_size = (int32_t)(10 + rand_double() * 990);
    msg->volume = (int32_t)(10000 + rand_double() * 990000);
    msg->timestamp_ns = now_nanos();
    strncpy(msg->symbol, "AAPL", FEED_MAX_SYMBOL - 1);
    msg->symbol[FEED_MAX_SYMBOL - 1] = '\0';
}

/* ---- Receive thread ---- */

static void *recv_thread_func(void *arg) {
    feed_handler_t *fh = (feed_handler_t *)arg;
    feed_msg_t msg_buf;

    if (fh->config.simulation_mode) {
        /* Simulation mode: generate synthetic messages */
        fh->sim_base_price = 150.0; /* Starting price for AAPL */
        uint32_t interval = fh->config.sim_interval_us;

        while (fh->running) {
            simulate_message(fh, &msg_buf);

            /* Allocate a copy for the queue */
            feed_msg_t *msg_copy = (feed_msg_t *)malloc(sizeof(feed_msg_t));
            if (!msg_copy) {
                fh->msgs_dropped++;
                sleep_us(interval);
                continue;
            }
            memcpy(msg_copy, &msg_buf, sizeof(feed_msg_t));

            if (!ring_buffer_push(&fh->output_queue, msg_copy)) {
                /* Queue full — drop the message */
                free(msg_copy);
                fh->msgs_dropped++;
            } else {
                fh->msgs_received++;
                fh->bytes_received += sizeof(feed_msg_t);
            }

            sleep_us(interval);
        }
    } else {
        /* Production mode: epoll-based UDP receiver */
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            fprintf(stderr, "feed_handler: epoll_create1 failed: %s\n",
                    strerror(errno));
            return NULL;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fh->sockfd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fh->sockfd, &ev) < 0) {
            fprintf(stderr, "feed_handler: epoll_ctl failed: %s\n",
                    strerror(errno));
            close(epfd);
            return NULL;
        }

        char raw_buf[FEED_MAX_MSG_SIZE];

        while (fh->running) {
            struct epoll_event events[64];
            int n = epoll_wait(epfd, events, 64, 100); /* 100ms timeout */

            for (int i = 0; i < n; i++) {
                if (events[i].events & EPOLLIN) {
                    ssize_t len = recv(events[i].data.fd, raw_buf,
                                       sizeof(raw_buf), 0);
                    if (len > 0) {
                        fh->bytes_received += (uint64_t)len;

                        /* Parse binary message into feed_msg_t.
                         * In production, this would decode a real protocol.
                         * Here we handle a simple struct copy for demo purposes. */
                        if ((size_t)len >= sizeof(feed_msg_t)) {
                            feed_msg_t *msg_copy = (feed_msg_t *)malloc(sizeof(feed_msg_t));
                            if (msg_copy) {
                                memcpy(msg_copy, raw_buf, sizeof(feed_msg_t));
                                if (!ring_buffer_push(&fh->output_queue, msg_copy)) {
                                    free(msg_copy);
                                    fh->msgs_dropped++;
                                } else {
                                    fh->msgs_received++;
                                }
                            } else {
                                fh->msgs_dropped++;
                            }
                        }
                    }
                }
            }
        }

        close(epfd);
    }

    return NULL;
}

/* ---- Public API ---- */

feed_handler_t *feed_init(const feed_config_t *config) {
    if (!config) return NULL;

    feed_handler_t *fh = (feed_handler_t *)calloc(1, sizeof(feed_handler_t));
    if (!fh) return NULL;

    fh->config = *config;

    if (ring_buffer_init(&fh->output_queue, config->output_queue_size) != 0) {
        free(fh);
        return NULL;
    }

    /* Set up UDP socket (production mode) */
    if (!config->simulation_mode) {
        fh->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fh->sockfd < 0) {
            ring_buffer_destroy(&fh->output_queue);
            free(fh);
            return NULL;
        }

        /* Set socket buffer size */
        int rcvbuf = 4 * 1024 * 1024; /* 4 MB */
        setsockopt(fh->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config->listen_port);
        if (inet_pton(AF_INET, config->listen_addr, &addr.sin_addr) <= 0) {
            close(fh->sockfd);
            ring_buffer_destroy(&fh->output_queue);
            free(fh);
            return NULL;
        }

        if (bind(fh->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fh->sockfd);
            ring_buffer_destroy(&fh->output_queue);
            free(fh);
            return NULL;
        }
    }

    return fh;
}

int feed_start(feed_handler_t *fh) {
    if (!fh) return -1;

    fh->running = true;
    if (pthread_create(&fh->recv_thread, NULL, recv_thread_func, fh) != 0) {
        fh->running = false;
        return -1;
    }

    return 0;
}

void feed_stop(feed_handler_t *fh) {
    if (!fh) return;
    fh->running = false;
    pthread_join(fh->recv_thread, NULL);
}

void feed_destroy(feed_handler_t *fh) {
    if (!fh) return;

    /* Drain and free any remaining messages in the queue */
    feed_msg_t *msg;
    while ((msg = (feed_msg_t *)ring_buffer_pop(&fh->output_queue)) != NULL) {
        free(msg);
    }

    ring_buffer_destroy(&fh->output_queue);

    if (fh->sockfd > 0) {
        close(fh->sockfd);
    }

    free(fh);
}

ring_buffer_t *feed_get_output_queue(feed_handler_t *fh) {
    return fh ? &fh->output_queue : NULL;
}

void feed_get_stats(feed_handler_t *fh, uint64_t *msgs_received,
                    uint64_t *msgs_dropped, uint64_t *bytes_received) {
    if (!fh) return;
    if (msgs_received)  *msgs_received  = fh->msgs_received;
    if (msgs_dropped)   *msgs_dropped   = fh->msgs_dropped;
    if (bytes_received) *bytes_received = fh->bytes_received;
}
