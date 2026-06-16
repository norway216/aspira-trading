/**
 * logger.h — Async, Low-Latency Logger
 *
 * Log messages are pushed to a lock-free ring buffer and consumed
 * by a dedicated background thread that writes them to disk.
 * The hot path (log_xxx calls) never blocks on I/O.
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log severity levels */
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4
} log_level_t;

/* Maximum length of a single log message */
#define LOG_MAX_MSG_LEN 512

/**
 * A single log entry stored in the ring buffer.
 */
typedef struct {
    uint64_t timestamp_ns;      /* nanosecond timestamp */
    log_level_t level;
    char message[LOG_MAX_MSG_LEN];
    uint16_t msg_len;           /* actual message length */
} log_entry_t;

/**
 * Async logger handle.
 * Opaque — use logger_init/logger_shutdown/logger_destroy.
 */
typedef struct logger_t logger_t;

/**
 * Create and start the async logger.
 * @param filepath  Path to log file (NULL or "-" for stdout)
 * @param queue_size  Number of log entries to buffer (> 0, power of 2 recommended)
 * @return Handle, or NULL on error
 */
logger_t *logger_init(const char *filepath, uint32_t queue_size);

/**
 * Signal the logger to stop, flush remaining entries, and join the thread.
 * Returns after all pending messages are written.
 */
void logger_shutdown(logger_t *log);

/**
 * Free logger resources. Call after logger_shutdown().
 */
void logger_destroy(logger_t *log);

/* ---- Hot-path logging functions (lock-free push) ---- */

/** Log a formatted message at the given level. */
void logger_write(logger_t *log, log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Convenience macros that compile away at lower log levels.
 *  Set LOG_ACTIVE_LEVEL at compile time to filter at compile time. */
#ifndef LOG_ACTIVE_LEVEL
#define LOG_ACTIVE_LEVEL LOG_TRACE
#endif

#define LOG_TRACE_(log, fmt, ...) logger_write(log, LOG_TRACE, "[TRACE] " fmt, ##__VA_ARGS__)
#define LOG_DEBUG_(log, fmt, ...) logger_write(log, LOG_DEBUG, "[DEBUG] " fmt, ##__VA_ARGS__)
#define LOG_INFO_(log, fmt, ...)  logger_write(log, LOG_INFO,  "[INFO]  " fmt, ##__VA_ARGS__)
#define LOG_WARN_(log, fmt, ...)  logger_write(log, LOG_WARN,  "[WARN]  " fmt, ##__VA_ARGS__)
#define LOG_ERROR_(log, fmt, ...) logger_write(log, LOG_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
