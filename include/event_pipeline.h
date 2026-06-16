/**
 * event_pipeline.h — Event-Driven Processing Pipeline
 *
 * Orchestrates the flow:
 *   Market Data → Parser → Order Book → Strategy → Risk → Execution
 *
 * Each stage runs in its own thread with lock-free queues between them.
 * This design ensures:
 *   - Pipelined parallelism (stages overlap)
 *   - Deterministic ordering per-symbol
 *   - Back-pressure via queue-full dropping
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

    /* Thread for consuming feed messages */
    pthread_t consumer_thread_;
    volatile bool running_;

    /* Stats */
    PipelineStats stats_;

    /* Order ID counter */
    volatile int32_t next_order_id_;

    /* Strategy: generate orders from market data */
    void strategy_on_market_data(const feed_msg_t &msg,
                                  std::vector<Order> &out_orders);

    /* Consumer thread function */
    static void *consumer_thread_func(void *arg);
    void consumer_loop();
};

#endif /* EVENT_PIPELINE_H */
