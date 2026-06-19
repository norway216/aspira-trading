/**
 * logger.c — Async Logger Implementation
 *
 * Two-ring-buffer architecture (correct SPSC design):
 *   free_queue_:   pool of pre-allocated log_entry_t (hot path pops, I/O pushes)
 *   filled_queue_: filled entries awaiting I/O (hot path pushes, I/O pops)
 *
 * Each ring buffer has exactly ONE producer and ONE consumer, avoiding
 * the race condition inherent in single-queue double-ended designs.
 *
 * Hot path  →  pop from free_queue_ → fill → push to filled_queue_  (never blocks)
 * I/O thread → pop from filled_queue_ → write → push to free_queue_
 */
#include "logger.h"
#include "ring_buffer.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct logger_t {
    ring_buffer_t free_queue_;    /* free entries (hot path pops, I/O thread pushes) */
    ring_buffer_t filled_queue_;  /* filled entries (hot path pushes, I/O thread pops) */
    FILE *file;
    bool running;
    bool owns_file;
    pthread_t thread;
    log_entry_t *entry_pool;      /* Pre-allocated entries, queue_size total */
};

/* ---- Helpers ---- */

static uint64_t now_nanos(void) {
    struct timespec ts;
    /* CLOCK_REALTIME — logger needs wall-clock timestamps for human-readable
     * log output and correlation with real-world events. Other components use
     * CLOCK_MONOTONIC for latency measurement where ordering matters. */
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *level_str(log_level_t level) {
    switch (level) {
    case LOG_TRACE: return "TRACE";
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "?????";
    }
}

static void flush_entry(log_entry_t *entry, FILE *file) {
    if (!entry || !file || entry->msg_len == 0) return;

    time_t sec = (time_t)(entry->timestamp_ns / 1000000000ULL);
    long ns = (long)(entry->timestamp_ns % 1000000000ULL);

    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(file, "%s.%09ld [%s] %s\n",
            time_buf, ns, level_str(entry->level), entry->message);
}

/* Flush every N entries to batch I/O */
#define LOG_FLUSH_BATCH 32

static void *logger_thread(void *arg) {
    logger_t *log = (logger_t *)arg;
    int batch_count = 0;

    while (log->running) {
        /* Pop a filled entry from the filled queue */
        log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->filled_queue_);
        if (entry) {
            flush_entry(entry, log->file);

            /* Clear the message length so stale data isn't written */
            entry->msg_len = 0;

            /* Return the entry to the free pool for reuse by the hot path */
            ring_buffer_push(&log->free_queue_, entry);

            /* Batch flush: every N entries to reduce syscall overhead */
            batch_count++;
            if (batch_count >= LOG_FLUSH_BATCH) {
                fflush(log->file);
                batch_count = 0;
            }
        } else {
            /* Flush any pending output when the queue is momentarily empty */
            if (batch_count > 0) {
                fflush(log->file);
                batch_count = 0;
            }
            /* Brief sleep to avoid busy-waiting */
            usleep(100);
        }
    }

    /* Drain remaining filled entries on shutdown */
    for (;;) {
        log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->filled_queue_);
        if (!entry) break;
        flush_entry(entry, log->file);
        /* Don't return to free queue during shutdown — we're cleaning up */
    }
    fflush(log->file);

    return NULL;
}

/* ---- Public API ---- */

logger_t *logger_init(const char *filepath, uint32_t queue_size) {
    logger_t *log = (logger_t *)calloc(1, sizeof(logger_t));
    if (!log) return NULL;

    /* Open log file */
    if (!filepath || strcmp(filepath, "-") == 0) {
        log->file = stdout;
        log->owns_file = false;
    } else {
        log->file = fopen(filepath, "a");
        if (!log->file) {
            free(log);
            return NULL;
        }
        log->owns_file = true;
    }

    /* Initialize two ring buffers — one for free entries, one for filled.
     * Each is strictly SPSC, guaranteeing no data races on head/tail. */
    if (ring_buffer_init(&log->free_queue_, queue_size) != 0) {
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }
    if (ring_buffer_init(&log->filled_queue_, queue_size) != 0) {
        ring_buffer_destroy(&log->free_queue_);
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }

    /* Pre-allocate log entry pool.
     * All entries start in free_queue_ — the hot path pops a free entry,
     * fills it, and pushes to filled_queue_. The I/O thread pops from
     * filled_queue_, writes to disk, and pushes back to free_queue_. */
    log->entry_pool = (log_entry_t *)calloc(queue_size, sizeof(log_entry_t));
    if (!log->entry_pool) {
        ring_buffer_destroy(&log->free_queue_);
        ring_buffer_destroy(&log->filled_queue_);
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }
    for (uint32_t i = 0; i < queue_size; i++) {
        ring_buffer_push(&log->free_queue_, &log->entry_pool[i]);
    }

    /* Start background I/O thread */
    log->running = true;
    if (pthread_create(&log->thread, NULL, logger_thread, log) != 0) {
        log->running = false;
        ring_buffer_destroy(&log->free_queue_);
        ring_buffer_destroy(&log->filled_queue_);
        free(log->entry_pool);
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }

    return log;
}

void logger_shutdown(logger_t *log) {
    if (!log) return;
    log->running = false;
    pthread_join(log->thread, NULL);
}

void logger_destroy(logger_t *log) {
    if (!log) return;
    ring_buffer_destroy(&log->free_queue_);
    ring_buffer_destroy(&log->filled_queue_);
    free(log->entry_pool);
    if (log->owns_file) fclose(log->file);
    free(log);
}

void logger_write(logger_t *log, log_level_t level, const char *fmt, ...) {
    if (!log || !fmt) return;

    /* Pop a free entry from the free queue (hot path is the consumer here) */
    log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->free_queue_);
    if (!entry) {
        /* All entries are currently filled and waiting to be written.
         * Drop this message — the hot path never blocks on I/O. */
        return;
    }

    entry->timestamp_ns = now_nanos();
    entry->level = level;

    va_list args;
    va_start(args, fmt);
    entry->msg_len = (uint16_t)vsnprintf(entry->message, LOG_MAX_MSG_LEN, fmt, args);
    va_end(args);

    /* Push the filled entry to the filled queue for the I/O thread */
    ring_buffer_push(&log->filled_queue_, entry);
}
