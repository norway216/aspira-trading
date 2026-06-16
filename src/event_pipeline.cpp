/**
 * event_pipeline.cpp — Event Pipeline Implementation
 *
 * This is the main orchestration layer that wires together:
 *   Feed Handler → Strategy → Risk → Order Book → Execution Gateway
 */
#include "event_pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

/* ---- Helper: generate a unique order ID ---- */

static int32_t generate_order_id(volatile int32_t &counter) {
    return __sync_fetch_and_add(&counter, 1);
}

/* ---- Construction / Destruction ---- */

EventPipeline::EventPipeline()
    : logger_(nullptr),
      journal_(nullptr),
      order_book_(nullptr),
      risk_engine_(nullptr),
      gateway_(nullptr),
      feed_handler_(nullptr),
      running_(false),
      next_order_id_(1)
{
    memset(&stats_, 0, sizeof(stats_));
}

EventPipeline::~EventPipeline() {
    stop();
}

/* ---- Initialization ---- */

int EventPipeline::init(const std::string &symbol,
                         const std::string &log_path) {
    /* 1. Logger */
    logger_ = logger_init(log_path.c_str(), 4096);
    if (!logger_) {
        fprintf(stderr, "EventPipeline: failed to initialize logger\n");
        return -1;
    }
    LOG_INFO_(logger_, "EventPipeline initializing for symbol %s", symbol.c_str());

    /* 2. Order Book */
    order_book_ = new OrderBook(symbol);
    if (!order_book_) {
        LOG_ERROR_(logger_, "Failed to create order book");
        return -1;
    }

    /* 3. Risk Engine */
    risk_engine_ = new RiskEngine();
    if (!risk_engine_) {
        LOG_ERROR_(logger_, "Failed to create risk engine");
        return -1;
    }

    /* Configure risk limits — generous for demo purposes */
    RiskLimits limits;
    limits.max_order_qty     = 10000;
    limits.max_position      = 10000000;
    limits.max_exposure      = 50000000;
    limits.max_notional      = 500000000.0;
    limits.price_band_pct    = 10.0;   /* 10% from mid */
    limits.max_orders_per_sec = 50000;
    limits.enabled           = true;
    risk_engine_->set_limits(limits);

    /* 4. Execution Gateway (simulation mode) */
    gw_config_t gw_cfg;
    memset(&gw_cfg, 0, sizeof(gw_cfg));
    gw_cfg.simulation_mode = true;
    strncpy(gw_cfg.sim_log_file, "execution_gateway.log",
            sizeof(gw_cfg.sim_log_file) - 1);

    gateway_ = gw_init(&gw_cfg);
    if (!gateway_) {
        LOG_ERROR_(logger_, "Failed to create execution gateway");
        return -1;
    }

    /* 5. Feed Handler (simulation mode) */
    feed_config_t feed_cfg;
    memset(&feed_cfg, 0, sizeof(feed_cfg));
    feed_cfg.simulation_mode = true;
    feed_cfg.sim_interval_us = 500;    /* 2000 msgs/sec */
    feed_cfg.output_queue_size = 4096;

    feed_handler_ = feed_init(&feed_cfg);
    if (!feed_handler_) {
        LOG_ERROR_(logger_, "Failed to create feed handler");
        return -1;
    }

    LOG_INFO_(logger_, "EventPipeline initialized successfully");
    return 0;
}

void EventPipeline::set_journal(journal_t *jrnl) {
    journal_ = jrnl;
}

/* ---- Strategy Logic ---- */

void EventPipeline::strategy_on_market_data(const feed_msg_t &msg,
                                             Order *out_orders, int *out_count) {
    /* Simple market-making-like strategy.
     * Writes up to 2 orders into the pre-allocated out_orders array. */

    double mid = (msg.bid_price + msg.ask_price) / 2.0;
    double spread = msg.ask_price - msg.bid_price;
    int count = 0;

    /* Place a buy order slightly below mid */
    {
        Order &buy_order = out_orders[count++];
        buy_order.id = generate_order_id(next_order_id_);
        buy_order.account_id = 1;
        buy_order.price = mid - spread * 0.1;
        buy_order.quantity = 100;
        buy_order.side = Side::BUY;
        buy_order.type = OrderType::LIMIT;
        buy_order.timestamp_ns = msg.timestamp_ns;
        memcpy(buy_order.symbol, msg.symbol, sizeof(buy_order.symbol));
    }

    /* Place a sell order slightly above mid */
    {
        Order &sell_order = out_orders[count++];
        sell_order.id = generate_order_id(next_order_id_);
        sell_order.account_id = 1;
        sell_order.price = mid + spread * 0.1;
        sell_order.quantity = 100;
        sell_order.side = Side::SELL;
        sell_order.type = OrderType::LIMIT;
        sell_order.timestamp_ns = msg.timestamp_ns;
        memcpy(sell_order.symbol, msg.symbol, sizeof(sell_order.symbol));
    }

    *out_count = count;
}

/* ---- Hot Path ---- */

void EventPipeline::process_market_data(feed_msg_t *msg) {
    if (!msg) return;

#ifdef TRADING_MEASURE_LATENCY
    uint64_t start_ns;
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    start_ns = (uint64_t)ts_start.tv_sec * 1000000000ULL + (uint64_t)ts_start.tv_nsec;
#endif

    stats_.market_data_msgs++;

    /* Stage 1: Strategy generates orders from market data */
    Order orders[2];       /* stack-allocated: strategy produces exactly 2 orders */
    int order_count = 0;
    strategy_on_market_data(*msg, orders, &order_count);

    for (int oi = 0; oi < order_count; oi++) {
        Order &order = orders[oi];
        stats_.orders_created++;

        /* Stage 2: Risk validation */
        double mid = order_book_->mid_price();
        if (!risk_engine_->validate(order, mid)) {
            stats_.orders_rejected++;
            LOG_WARN_(logger_, "Order %d REJECTED: %s",
                      order.id, risk_engine_->last_reject_reason().c_str());
            /* Journal the rejected order */
            if (journal_) {
                journal_log_order(journal_, JRNL_ORDER_REJECT,
                                  order.id, order.account_id,
                                  order.price, order.quantity, 0,
                                  order.timestamp_ns,
                                  (uint8_t)(order.is_buy() ? 0 : 1),
                                  (uint8_t)(order.type == OrderType::LIMIT ? 0 : 1),
                                  (uint8_t)OrderStatus::REJECTED,
                                  order.symbol);
            }
            continue;
        }
        stats_.orders_accepted++;

        /* Stage 3: Submit to order book */
        Order result = order_book_->add_order(order);

        /* Journal the accepted order */
        if (journal_) {
            journal_log_order(journal_, JRNL_ORDER_NEW,
                              result.id, result.account_id,
                              result.price, result.quantity,
                              result.filled_qty,
                              result.timestamp_ns,
                              (uint8_t)(result.is_buy() ? 0 : 1),
                              (uint8_t)(result.type == OrderType::LIMIT ? 0 : 1),
                              (uint8_t)result.status,
                              result.symbol);
        }

        /* Stage 4: Handle fills */
        if (result.filled_qty > 0) {
            risk_engine_->on_order_executed(result, result.filled_qty,
                                             result.price > 0 ? result.price : mid);

            LOG_INFO_(logger_, "Order %d FILLED: %s %d@%.2f status=%d",
                      result.id,
                      result.is_buy() ? "BUY" : "SELL",
                      result.filled_qty, result.price,
                      (int)result.status);

            /* Journal the fill */
            if (journal_) {
                journal_log_order(journal_, JRNL_ORDER_FILLED,
                                  result.id, result.account_id,
                                  result.price, result.quantity,
                                  result.filled_qty,
                                  result.timestamp_ns,
                                  (uint8_t)(result.is_buy() ? 0 : 1),
                                  (uint8_t)(result.type == OrderType::LIMIT ? 0 : 1),
                                  (uint8_t)result.status,
                                  result.symbol);
            }
        }

        /* Stage 5: Send to execution gateway */
        if (result.status == OrderStatus::NEW ||
            result.status == OrderStatus::PARTIAL) {
            char gw_msg[GW_MAX_MSG_SIZE];
            char side_ch = result.is_buy() ? 'B' : 'S';
            char type_ch = (result.type == OrderType::LIMIT) ? 'L' : 'M';

            if (gw_serialize_order(gw_msg, sizeof(gw_msg),
                                    result.id, result.symbol,
                                    side_ch, type_ch,
                                    result.price, result.quantity,
                                    result.timestamp_ns) == 0) {
                gw_send_order(gateway_, gw_msg, (uint32_t)strlen(gw_msg),
                              result.id);
                stats_.orders_executed++;
            }
        }

        /* Check for trades generated */
        std::vector<Trade> trades = order_book_->drain_trades();
        for (const auto &trade : trades) {
            stats_.trades_generated++;
            LOG_INFO_(logger_,
                      "TRADE id=%d %s %d@%.2f (buy=%d, sell=%d)",
                      trade.trade_id, trade.symbol,
                      trade.quantity, trade.price,
                      trade.buy_order_id, trade.sell_order_id);
            /* Journal the trade */
            if (journal_) {
                journal_log_trade(journal_,
                                  trade.trade_id,
                                  trade.buy_order_id,
                                  trade.sell_order_id,
                                  trade.price,
                                  trade.quantity,
                                  trade.timestamp_ns,
                                  trade.symbol);
            }
        }
    }

    /* Return message to feed handler's memory pool */
    feed_return_msg(feed_handler_, msg);

#ifdef TRADING_MEASURE_LATENCY
    /* Latency measurement — 2 clock_gettime syscalls per message.
     * Disable with -DTRADING_NO_MEASURE_LATENCY for production. */
    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t end_ns = (uint64_t)ts_end.tv_sec * 1000000000ULL +
                      (uint64_t)ts_end.tv_nsec;

    double latency_us = (double)(end_ns - start_ns) / 1000.0;
    if (stats_.avg_latency_us == 0.0) {
        stats_.avg_latency_us = latency_us;
    } else {
        stats_.avg_latency_us = stats_.avg_latency_us * 0.9 + latency_us * 0.1;
    }
#endif
}

/* ---- Consumer Thread ---- */

void EventPipeline::consumer_loop() {
    ring_buffer_t *queue = feed_get_output_queue(feed_handler_);

    while (running_) {
        feed_msg_t *msg = (feed_msg_t *)ring_buffer_pop(queue);
        if (msg) {
            process_market_data(msg);
        } else {
            /* No data — brief yield to avoid busy-waiting */
            usleep(50); /* 50 µs */
        }
    }

    /* Drain remaining messages on stop */
    {
        feed_msg_t *drain_msg;
        while ((drain_msg = (feed_msg_t *)ring_buffer_pop(queue)) != NULL) {
            process_market_data(drain_msg);
        }
    }
}

void *EventPipeline::consumer_thread_func(void *arg) {
    EventPipeline *pipeline = static_cast<EventPipeline *>(arg);
    pipeline->consumer_loop();
    return nullptr;
}

/* ---- Start / Stop ---- */

int EventPipeline::start() {
    if (running_) return 0;

    LOG_INFO_(logger_, "EventPipeline starting...");

    /* Start feed handler (generates market data) */
    if (feed_start(feed_handler_) != 0) {
        LOG_ERROR_(logger_, "Failed to start feed handler");
        return -1;
    }

    /* Start consumer thread */
    running_ = true;
    if (pthread_create(&consumer_thread_, nullptr,
                       consumer_thread_func, this) != 0) {
        running_ = false;
        feed_stop(feed_handler_);
        LOG_ERROR_(logger_, "Failed to create consumer thread");
        return -1;
    }

    LOG_INFO_(logger_, "EventPipeline started — processing events");
    return 0;
}

void EventPipeline::stop() {
    if (!running_) return;

    LOG_INFO_(logger_, "EventPipeline stopping...");

    /* Stop consumer thread */
    running_ = false;
    pthread_join(consumer_thread_, nullptr);

    /* Stop feed handler */
    feed_stop(feed_handler_);

    /* Shutdown gateway */
    gw_shutdown(gateway_);

    /* Log final stats */
    LOG_INFO_(logger_, "=== Final Pipeline Statistics ===");
    LOG_INFO_(logger_, "  Market data msgs:  %lu", (unsigned long)stats_.market_data_msgs);
    LOG_INFO_(logger_, "  Orders created:    %lu", (unsigned long)stats_.orders_created);
    LOG_INFO_(logger_, "  Orders accepted:   %lu", (unsigned long)stats_.orders_accepted);
    LOG_INFO_(logger_, "  Orders rejected:   %lu", (unsigned long)stats_.orders_rejected);
    LOG_INFO_(logger_, "  Orders executed:   %lu", (unsigned long)stats_.orders_executed);
    LOG_INFO_(logger_, "  Trades generated:  %lu", (unsigned long)stats_.trades_generated);
    LOG_INFO_(logger_, "  Avg latency (us):  %.2f", stats_.avg_latency_us);

    /* Shutdown logger */
    logger_shutdown(logger_);
}

/* ---- Accessors ---- */

PipelineStats EventPipeline::stats() const {
    return stats_;
}

const OrderBook &EventPipeline::order_book() const {
    return *order_book_;
}

RiskEngine &EventPipeline::risk_engine() {
    return *risk_engine_;
}
