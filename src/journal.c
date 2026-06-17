/**
 * journal.c — Append-Only Persistence Journal Implementation
 *
 * Supports both regular file I/O and mmap for high-throughput writes.
 * CRC32 is computed over header + payload for data integrity.
 */
#include "journal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Default mmap region size (64 MB) */
#define DEFAULT_MMAP_SIZE (64ULL * 1024 * 1024)

struct journal_t {
    int fd;
    FILE *file;
    bool use_mmap;
    uint64_t sequence;
    uint64_t bytes_written;
    uint64_t file_size;

    /* mmap state */
    void *mmap_base;
    uint64_t mmap_size;
    uint64_t mmap_offset; /* Write position within mmap region */
};

/* CRC32 table (IEEE 802.3) */
static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void crc32_init(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

/* Compute CRC32 over data, starting from initial value (0xFFFFFFFF for new CRC) */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    }
    return crc;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    return crc32_update(0xFFFFFFFF, data, len) ^ 0xFFFFFFFF;
}

static uint64_t now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- Public API ---- */

journal_t *journal_open(const char *filepath, bool use_mmap) {
    if (!filepath) return NULL;

    journal_t *jrnl = (journal_t *)calloc(1, sizeof(journal_t));
    if (!jrnl) return NULL;

    jrnl->use_mmap = use_mmap;

    if (use_mmap) {
        jrnl->fd = open(filepath, O_RDWR | O_CREAT, 0644);
        if (jrnl->fd < 0) {
            free(jrnl);
            return NULL;
        }

        /* Get current file size */
        struct stat st;
        if (fstat(jrnl->fd, &st) == 0) {
            jrnl->file_size = (uint64_t)st.st_size;
        }

        /* Extend to mmap size if needed */
        jrnl->mmap_size = DEFAULT_MMAP_SIZE;
        if (jrnl->file_size < jrnl->mmap_size) {
            if (ftruncate(jrnl->fd, (off_t)jrnl->mmap_size) < 0) {
                close(jrnl->fd);
                free(jrnl);
                return NULL;
            }
            jrnl->file_size = jrnl->mmap_size;
        } else {
            jrnl->mmap_size = jrnl->file_size;
        }

        jrnl->mmap_base = mmap(NULL, (size_t)jrnl->mmap_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, jrnl->fd, 0);
        if (jrnl->mmap_base == MAP_FAILED) {
            close(jrnl->fd);
            free(jrnl);
            return NULL;
        }

        jrnl->mmap_offset = jrnl->file_size > 0 ? jrnl->file_size : 0;
    } else {
        jrnl->file = fopen(filepath, "ab");
        if (!jrnl->file) {
            free(jrnl);
            return NULL;
        }
        jrnl->fd = fileno(jrnl->file);
    }

    return jrnl;
}

void journal_close(journal_t *jrnl) {
    if (!jrnl) return;

    journal_sync(jrnl);

    if (jrnl->use_mmap) {
        if (jrnl->mmap_base && jrnl->mmap_base != MAP_FAILED) {
            munmap(jrnl->mmap_base, (size_t)jrnl->mmap_size);
        }
        if (jrnl->fd >= 0) {
            /* Truncate to actual used size */
            ftruncate(jrnl->fd, (off_t)jrnl->mmap_offset);
            close(jrnl->fd);
        }
    } else {
        if (jrnl->file) {
            fclose(jrnl->file);
        }
    }

    free(jrnl);
}

int journal_append(journal_t *jrnl, journal_record_type_t type,
                   const void *payload, uint32_t payload_size) {
    if (!jrnl) return -1;

    /* Build header (no memset — all fields are set explicitly) */
    journal_header_t hdr;
    hdr.timestamp_ns     = now_nanos();
    hdr.record_type      = (uint32_t)type;
    hdr.payload_size     = payload_size;
    hdr.sequence_number  = jrnl->sequence++;
    hdr.crc32            = 0;
    hdr._reserved        = 0;

    /* Compute CRC32 incrementally: header (CRC=0) then payload.
     * No malloc/free needed — two-pass CRC using the same state. */
    uint32_t crc = crc32_update(0xFFFFFFFF, &hdr, sizeof(hdr));
    if (payload && payload_size > 0) {
        crc = crc32_update(crc, payload, payload_size);
    }
    hdr.crc32 = crc ^ 0xFFFFFFFF;

    /* Write to journal */
    size_t total_size = sizeof(hdr) + payload_size;

    if (jrnl->use_mmap) {
        /* Check if we need to extend the mmap region */
        if (jrnl->mmap_offset + total_size > jrnl->mmap_size) {
            /* Extend by another DEFAULT_MMAP_SIZE */
            uint64_t new_size = jrnl->mmap_size + DEFAULT_MMAP_SIZE;
            if (ftruncate(jrnl->fd, (off_t)new_size) < 0) {
                return -1;
            }
            munmap(jrnl->mmap_base, (size_t)jrnl->mmap_size);
            jrnl->mmap_base = mmap(NULL, (size_t)new_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, jrnl->fd, 0);
            if (jrnl->mmap_base == MAP_FAILED) {
                return -1;
            }
            jrnl->mmap_size = new_size;
        }

        uint8_t *dest = (uint8_t *)jrnl->mmap_base + jrnl->mmap_offset;
        memcpy(dest, &hdr, sizeof(hdr));
        if (payload && payload_size > 0) {
            memcpy(dest + sizeof(hdr), payload, payload_size);
        }
        jrnl->mmap_offset += total_size;
    } else {
        size_t written = fwrite(&hdr, 1, sizeof(hdr), jrnl->file);
        if (written != sizeof(hdr)) return -1;

        if (payload && payload_size > 0) {
            written = fwrite(payload, 1, payload_size, jrnl->file);
            if (written != payload_size) return -1;
        }
    }

    jrnl->bytes_written += total_size;
    return 0;
}

void journal_sync(journal_t *jrnl) {
    if (!jrnl) return;

    if (jrnl->use_mmap) {
        msync(jrnl->mmap_base, (size_t)jrnl->mmap_offset, MS_SYNC);
    } else {
        fflush(jrnl->file);
        fsync(jrnl->fd);
    }
}

uint64_t journal_sequence(const journal_t *jrnl) {
    return jrnl ? jrnl->sequence : 0;
}

uint64_t journal_bytes_written(const journal_t *jrnl) {
    return jrnl ? jrnl->bytes_written : 0;
}

/* ---- Convenience functions ---- */

int journal_log_order(journal_t *jrnl, journal_record_type_t type,
                      int32_t order_id, int32_t account_id,
                      double price, int32_t qty, int32_t filled_qty,
                      uint64_t order_ts_ns, uint8_t side, uint8_t order_type,
                      uint8_t status, const char *symbol) {
    journal_order_payload_t payload;
    /* Set all fields explicitly — no memset needed.
     * The _pad byte and any trailing bytes in symbol[] are
     * uninitialized but won't be read by journal_append. */
    payload.order_id = order_id;
    payload.account_id = account_id;
    payload.price = price;
    payload.quantity = qty;
    payload.filled_qty = filled_qty;
    payload.order_ts_ns = order_ts_ns;
    payload.side = side;
    payload.order_type = order_type;
    payload.status = status;
    payload._pad = 0;
    if (symbol) {
        strncpy(payload.symbol, symbol, sizeof(payload.symbol) - 1);
        payload.symbol[sizeof(payload.symbol) - 1] = '\0';
    } else {
        payload.symbol[0] = '\0';
    }

    return journal_append(jrnl, type, &payload, sizeof(payload));
}

int journal_log_trade(journal_t *jrnl, int32_t trade_id,
                      int32_t buy_order_id, int32_t sell_order_id,
                      double price, int32_t qty, uint64_t trade_ts_ns,
                      const char *symbol) {
    journal_trade_payload_t payload;
    /* Set all fields explicitly — no memset needed */
    payload.trade_id = trade_id;
    payload.buy_order_id = buy_order_id;
    payload.sell_order_id = sell_order_id;
    payload.price = price;
    payload.quantity = qty;
    payload.trade_ts_ns = trade_ts_ns;
    memset(payload._pad, 0, sizeof(payload._pad));
    if (symbol) {
        strncpy(payload.symbol, symbol, sizeof(payload.symbol) - 1);
        payload.symbol[sizeof(payload.symbol) - 1] = '\0';
    } else {
        payload.symbol[0] = '\0';
    }

    return journal_append(jrnl, JRNL_TRADE, &payload, sizeof(payload));
}
