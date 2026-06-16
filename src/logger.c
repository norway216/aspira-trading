/**
 * logger.c — Async Logger Implementation
 *
 * Architecture:
 *   Hot path  →  ring_buffer_push()  →  (lock-free)
 *   Background thread  →  ring_buffer_pop()  →  fwrite()
 */
#include "logger.h"
#include "ring_buffer.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Timespec helpers */
#include <time.h>

struct logger_t {
    ring_buffer_t queue;
    FILE *file;
    bool running;
    bool owns_file;
    pthread_t thread;
    log_entry_t *entry_pool;
};

/* ---- Background thread ---- */

static uint64_t now_nanos(void) {
    struct timespec ts;
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

    /* Convert nanosecond timestamp to human-readable */
    time_t sec = (time_t)(entry->timestamp_ns / 1000000000ULL);
    long ns = (long)(entry->timestamp_ns % 1000000000ULL);

    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(file, "%s.%09ld [%s] %s\n",
            time_buf, ns, level_str(entry->level), entry->message);
    /* fflush batched by caller — not per-entry */
}

#define LOG_FLUSH_BATCH 32  /* flush every N entries */
#define LOG_FLUSH_US   10000 /* or every 10ms */

static void *logger_thread(void *arg) {
    logger_t *log = (logger_t *)arg;
    int batch_count = 0;
    uint64_t last_flush_ns = 0;

    while (log->running) {
        log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->queue);
        if (entry) {
            flush_entry(entry, log->file);
            /* Return entry to the pool for reuse */
            entry->msg_len = 0;
            ring_buffer_push(&log->queue, entry);

            /* Batch flush: every N entries or after 10ms */
            batch_count++;
            if (batch_count >= LOG_FLUSH_BATCH) {
                fflush(log->file);
                batch_count = 0;
            }
        } else {
            /* Flush if enough time has passed since last flush */
            if (batch_count > 0) {
                fflush(log->file);
                batch_count = 0;
            }
            /* Queue empty — brief sleep to avoid busy-waiting */
            usleep(100); /* 100 µs */
        }
    }

    /* Flush remaining entries on shutdown */
    for (;;) {
        log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->queue);
        if (!entry) break;
        flush_entry(entry, log->file);
        entry->msg_len = 0;
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

    /* Initialize ring buffer */
    if (ring_buffer_init(&log->queue, queue_size) != 0) {
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }

    /* Pre-allocate log entry pool.
     * We allocate queue_size entries and push them all onto the queue
     * as free slots. The producer pops a free one, fills it, pushes it back.
     * The consumer pops a filled one, writes it, and pushes it back as free.
     *
     * This is a double-ended ring buffer pattern:
     *   - Producer pops "free" entry, fills it, pushes "filled" entry
     *   - Consumer pops "filled" entry, writes it, pushes "free" entry
     *
     * Both use the SAME ring buffer, so entries circulate.
     */
    log->entry_pool = (log_entry_t *)calloc(queue_size, sizeof(log_entry_t));
    if (!log->entry_pool) {
        ring_buffer_destroy(&log->queue);
        if (log->owns_file) fclose(log->file);
        free(log);
        return NULL;
    }
    for (uint32_t i = 0; i < queue_size; i++) {
        ring_buffer_push(&log->queue, &log->entry_pool[i]);
    }

    /* Start background thread */
    log->running = true;
    if (pthread_create(&log->thread, NULL, logger_thread, log) != 0) {
        log->running = false;
        ring_buffer_destroy(&log->queue);
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
    ring_buffer_destroy(&log->queue);
    free(log->entry_pool);
    if (log->owns_file) fclose(log->file);
    free(log);
}

void logger_write(logger_t *log, log_level_t level, const char *fmt, ...) {
    if (!log || !fmt) return;

    /* Pop a free entry from the ring buffer */
    log_entry_t *entry = (log_entry_t *)ring_buffer_pop(&log->queue);
    if (!entry) {
        /* Queue is full (all entries are filled and waiting to be written).
         * Drop this message — we never block the hot path. */
        return;
    }

    entry->timestamp_ns = now_nanos();
    entry->level = level;

    va_list args;
    va_start(args, fmt);
    entry->msg_len = (uint16_t)vsnprintf(entry->message, LOG_MAX_MSG_LEN, fmt, args);
    va_end(args);

    /* Push the filled entry back onto the ring buffer */
    ring_buffer_push(&log->queue, entry);
}
