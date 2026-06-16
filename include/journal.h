/**
 * journal.h — Append-Only Persistence Journal
 *
 * Records all orders and trades for recovery and audit.
 * Uses memory-mapped files (mmap) for high-throughput writes.
 *
 * Format: binary records with fixed-size headers.
 * Recovery: replay the journal from the beginning.
 * Snapshot: periodically write full state for faster recovery.
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record types */
typedef enum {
    JRNL_ORDER_NEW      = 1,
    JRNL_ORDER_FILLED   = 2,
    JRNL_ORDER_CANCEL   = 3,
    JRNL_ORDER_REJECT   = 4,
    JRNL_TRADE          = 5,
    JRNL_SNAPSHOT       = 99
} journal_record_type_t;

/**
 * Fixed-size journal record header.
 * All records start with this header, followed by type-specific payload.
 */
typedef struct {
    uint64_t timestamp_ns;       /* Record timestamp */
    uint32_t record_type;        /* journal_record_type_t */
    uint32_t payload_size;       /* Size of payload following this header */
    uint64_t sequence_number;    /* Monotonically increasing sequence */
    uint32_t crc32;              /* CRC32 of header + payload */
    uint32_t _reserved;
} __attribute__((packed)) journal_header_t;

/* Total header size: 8+4+4+8+4+4 = 32 bytes */

/**
 * Order record payload.
 */
typedef struct {
    int32_t  order_id;
    int32_t  account_id;
    double   price;
    int32_t  quantity;
    int32_t  filled_qty;
    uint64_t order_ts_ns;
    uint8_t  side;        /* 0=buy, 1=sell */
    uint8_t  order_type;  /* 0=limit, 1=market */
    uint8_t  status;      /* order status */
    uint8_t  _pad;
    char     symbol[16];
} __attribute__((packed)) journal_order_payload_t;

/**
 * Trade record payload.
 */
typedef struct {
    int32_t  trade_id;
    int32_t  buy_order_id;
    int32_t  sell_order_id;
    double   price;
    int32_t  quantity;
    uint64_t trade_ts_ns;
    char     symbol[16];
    char     _pad[4];
} __attribute__((packed)) journal_trade_payload_t;

/**
 * Journal handle.
 */
typedef struct journal_t journal_t;

/**
 * Open (or create) a journal file.
 * @param filepath Path to journal file
 * @param use_mmap Use memory-mapped I/O if true
 * @return Handle, or NULL on error
 */
journal_t *journal_open(const char *filepath, bool use_mmap);

/**
 * Close the journal (flushes and syncs).
 */
void journal_close(journal_t *jrnl);

/**
 * Append a raw record to the journal.
 * The header is filled in automatically; caller provides type + payload.
 * @return 0 on success, -1 on error
 */
int journal_append(journal_t *jrnl, journal_record_type_t type,
                   const void *payload, uint32_t payload_size);

/**
 * Force a sync to disk (fsync / msync).
 */
void journal_sync(journal_t *jrnl);

/**
 * Get the current sequence number.
 */
uint64_t journal_sequence(const journal_t *jrnl);

/**
 * Get the total bytes written.
 */
uint64_t journal_bytes_written(const journal_t *jrnl);

/**
 * Convenience: append an order record.
 */
int journal_log_order(journal_t *jrnl, journal_record_type_t type,
                      int32_t order_id, int32_t account_id,
                      double price, int32_t qty, int32_t filled_qty,
                      uint64_t order_ts_ns, uint8_t side, uint8_t order_type,
                      uint8_t status, const char *symbol);

/**
 * Convenience: append a trade record.
 */
int journal_log_trade(journal_t *jrnl, int32_t trade_id,
                      int32_t buy_order_id, int32_t sell_order_id,
                      double price, int32_t qty, uint64_t trade_ts_ns,
                      const char *symbol);

#ifdef __cplusplus
}
#endif

#endif /* JOURNAL_H */
