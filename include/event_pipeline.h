/**
 * event_pipeline.h — Event-Driven Processing Pipeline
 *
 * Orchestrates the flow:
 *   Market Data → Strategy → Risk → Order Book → Execution
 *
 * Two-stage pipeline architecture:
 *   Stage 1 (Hot Path): strategy + risk + order book — CPU-only, no I/O
 *   Stage 2 (I/O Thread): journal writes + logging + execution gateway
 *
 * The stages communicate via a lock-free SPSC ring buffer. This ensures
 * the hot path never blocks on I/O — even mmap journal writes and fprintf
 * are offloaded to the I/O thread.
 */
#ifndef EVENT_PIPELINE_H
#define EVENT_PIPELINE_H

#include "feed_handler.h"
#include "order.h"
#include "order_book.h"
#include "risk_engine.h"
#include "execution_gateway.h"
#include "ring_buffer.h"
#include <pthread.h>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include "logger.h"
#include "journal.h"
#ifdef __cplusplus
}
#endif

/**
 * Pipeline statistics.
 */
struct PipelineStats {
    uint64_t market_data_msgs;
    uint64_t orders_created;
    uint64_t orders_accepted;
    uint64_t orders_rejected;
    uint64_t trades_generated;
    uint64_t orders_executed;
    double   avg_latency_us;
};

/**
 * Event types for the hot-path → I/O-thread queue.
 * The hot path pushes these events; the I/O thread does journal writes,
 * logging, and gateway dispatch without blocking the critical path.
 */
enum class IOEventType : uint8_t {
    ORDER_ACCEPTED = 0,
    ORDER_REJECTED = 1,
    ORDER_FILLED   = 2,
    TRADE          = 3,
};

struct IOEvent {
    IOEventType type;

    /* Order fields (valid for ORDER_ACCEPTED/REJECTED/FILLED) */
    int32_t  order_id;
    int32_t  account_id;
    double   price;
    int32_t  quantity;
    int32_t  filled_qty;
    uint64_t timestamp_ns;
    uint8_t  side;        /* 0=buy, 1=sell */
    uint8_t  order_type;  /* 0=limit, 1=market */
    uint8_t  status;
    char     symbol[16];

    /* Reject reason (valid for ORDER_REJECTED) */
    char     reject_reason[256];

    /* Trade fields (valid for TRADE) */
    int32_t  trade_id;
    int32_t  buy_order_id;
    int32_t  sell_order_id;
    double   trade_price;
    int32_t  trade_qty;
    uint64_t trade_ts_ns;
    char     trade_symbol[16];

    /* Gateway dispatch */
    bool     send_to_gateway;
    char     gw_msg[GW_MAX_MSG_SIZE];
    uint32_t gw_msg_len;
};

/**
 * The event pipeline ties all components together.
 */
class EventPipeline {
public:
    EventPipeline();
    ~EventPipeline();

    /**
     * Initialize all pipeline components.
     * @param symbol      Trading symbol
     * @param log_path    Logger output path
     * @return 0 on success, -1 on error
     */
    int init(const std::string &symbol, const std::string &log_path);

    /**
     * Set the persistence journal for recording orders and trades.
     */
    void set_journal(journal_t *jrnl);

    /**
     * Start the pipeline (launches all threads).
     */
    int start();

    /**
     * Stop the pipeline (joins all threads, flushes).
     */
    void stop();

    /**
     * Get pipeline statistics.
     */
    PipelineStats stats() const;

    /**
     * Get the order book (read-only for external queries).
     */
    const OrderBook &order_book() const;

    /**
     * Get the risk engine.
     */
    RiskEngine &risk_engine();

    /**
     * Process a single market data event through the pipeline.
     * This is the core hot-path function — called from the feed consumer thread.
     */
    void process_market_data(feed_msg_t *msg);

private:
    /* Components */
    logger_t *logger_;
    journal_t *journal_;
    OrderBook *order_book_;
    RiskEngine *risk_engine_;
    execution_gateway_t *gateway_;

    /* Simulated feed */
    feed_handler_t *feed_handler_;

    /* Threads */
    pthread_t consumer_thread_;   /* Hot path: feed → strategy → risk → order book */
    pthread_t io_thread_;         /* I/O: journal writes, logging, gateway dispatch */
    volatile bool running_;

    /* Two separate ring buffers for the I/O pipeline — avoids the race
     * condition inherent in single-queue double-ended designs:
     *   io_free_queue_: pool of pre-allocated IOEvents (hot path pops, I/O thread pushes)
     *   io_filled_queue_: filled IOEvents awaiting I/O processing (hot path pushes, I/O thread pops)
     * This is the standard correct pattern for producer-consumer event passing. */
    ring_buffer_t io_free_queue_;
    ring_buffer_t io_filled_queue_;
    IOEvent *io_event_pool_;      /* Pre-allocated event objects */

    /* Stats */
    PipelineStats stats_;

    /* Order ID counter */
    volatile int32_t next_order_id_;

    /* Strategy: generate orders from market data into pre-allocated array.
     * Writes up to 2 orders to out_orders and sets *out_count. */
    void strategy_on_market_data(const feed_msg_t &msg,
                                  Order *out_orders, int *out_count);

    /* Thread functions */
    static void *consumer_thread_func(void *arg);
    static void *io_thread_func(void *arg);
    void consumer_loop();
    void io_loop();

    /* I/O event processing (called from I/O thread) */
    void process_io_event(const IOEvent &event);
};

#endif /* EVENT_PIPELINE_H */
