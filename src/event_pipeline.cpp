/**
 * event_pipeline.cpp — Event Pipeline Implementation
 *
 * Two-stage pipeline:
 *   Stage 1 (Hot Path / Consumer Thread):
 *     Feed → Strategy → Risk → Order Book → pushes IOEvent to io_queue_
 *     Zero I/O in the hot path — no journal writes, no logging, no fprintf.
 *
 *   Stage 2 (I/O Thread):
 *     Pops IOEvent from io_queue_ → journal log → logger → gateway dispatch.
 *     All I/O is batched in this thread, keeping the hot path deterministic.
 */
#include "event_pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

/* ---- Helper: generate a unique order ID (relaxed atomic — only atomicity needed) ---- */

static int32_t generate_order_id(std::atomic<int32_t> &counter) {
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/* ---- Construction / Destruction ---- */

EventPipeline::EventPipeline()
    : logger_(nullptr),
      journal_(nullptr),
      order_book_(nullptr),
      risk_engine_(nullptr),
      gateway_(nullptr),
      feed_handler_(nullptr),
      io_event_pool_(nullptr)
{
    memset(&stats_, 0, sizeof(stats_));
    memset(&io_free_queue_, 0, sizeof(io_free_queue_));
    memset(&io_filled_queue_, 0, sizeof(io_filled_queue_));
}

EventPipeline::~EventPipeline() {
    stop();

    /* Destroy resources in reverse order of creation */
    if (feed_handler_) {
        feed_destroy(feed_handler_);
        feed_handler_ = nullptr;
    }

    if (gateway_) {
        gw_destroy(gateway_);
        gateway_ = nullptr;
    }

    delete risk_engine_;
    risk_engine_ = nullptr;

    delete order_book_;
    order_book_ = nullptr;

    if (io_event_pool_) {
        /* Drain remaining events from the I/O queues before destroying them.
         * At this point the threads are stopped, so no concurrent access. */
        IOEvent *ev;
        while ((ev = (IOEvent *)ring_buffer_pop(&io_free_queue_)) != nullptr) {}
        while ((ev = (IOEvent *)ring_buffer_pop(&io_filled_queue_)) != nullptr) {}
        free(io_event_pool_);
        io_event_pool_ = nullptr;
    }

    ring_buffer_destroy(&io_filled_queue_);
    ring_buffer_destroy(&io_free_queue_);

    if (logger_) {
        logger_destroy(logger_);
        logger_ = nullptr;
    }
}

/* ---- Initialization ---- */

int EventPipeline::init(const std::string &symbol,
                         const std::string &log_path) {
    /* 1. Logger (async — ring buffer + background thread) */
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
    strncpy(feed_cfg.symbol, symbol.c_str(), FEED_MAX_SYMBOL - 1);

    feed_handler_ = feed_init(&feed_cfg);
    if (!feed_handler_) {
        LOG_ERROR_(logger_, "Failed to create feed handler");
        return -1;
    }

    /* 6. I/O queues (hot path ↔ I/O thread).
     * Two separate ring buffers — one for free events, one for filled events.
     * This avoids the race condition inherent in single-queue designs where
     * the producer could accidentally pop a filled entry it hasn't processed yet.
     *
     *   io_free_queue_:   free pool entries (hot path pops, I/O thread pushes)
     *   io_filled_queue_: filled events (hot path pushes, I/O thread pops)
     *
     * Sized generously to absorb bursts — matches feed output queue size. */
    if (ring_buffer_init(&io_free_queue_, 4096) != 0) {
        LOG_ERROR_(logger_, "Failed to create I/O free queue");
        return -1;
    }
    if (ring_buffer_init(&io_filled_queue_, 4096) != 0) {
        LOG_ERROR_(logger_, "Failed to create I/O filled queue");
        return -1;
    }

    /* Pre-allocate IOEvent pool.
     * All entries start in io_free_queue_ — the hot path pops a free event,
     * fills it, and pushes to io_filled_queue_. The I/O thread pops from
     * io_filled_queue_, processes, and pushes back to io_free_queue_. */
    io_event_pool_ = (IOEvent *)calloc(4096, sizeof(IOEvent));
    if (!io_event_pool_) {
        LOG_ERROR_(logger_, "Failed to allocate I/O event pool");
        return -1;
    }
    for (uint32_t i = 0; i < 4096; i++) {
        ring_buffer_push(&io_free_queue_, &io_event_pool_[i]);
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

/* ---- Hot Path (Stage 1: CPU-only, no I/O) ---- */

void EventPipeline::process_market_data(feed_msg_t *msg) {
    if (!msg) return;

#ifdef TRADING_MEASURE_LATENCY
    uint64_t start_ns;
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    start_ns = (uint64_t)ts_start.tv_sec * 1000000000ULL + (uint64_t)ts_start.tv_nsec;
#endif

    stats_.market_data_msgs++;

    /* Stage 1a: Strategy generates orders from market data */
    Order orders[2];       /* stack-allocated: strategy produces exactly 2 orders */
    int order_count = 0;
    strategy_on_market_data(*msg, orders, &order_count);

    for (int oi = 0; oi < order_count; oi++) {
        Order &order = orders[oi];
        stats_.orders_created++;

        /* Stage 1b: Risk validation */
        double mid = order_book_->mid_price();
        if (!risk_engine_->validate(order, mid)) {
            stats_.orders_rejected++;

            /* Pop a free IOEvent from the free queue and fill it */
            IOEvent *ioev = (IOEvent *)ring_buffer_pop(&io_free_queue_);
            if (ioev) {
                ioev->type = IOEventType::ORDER_REJECTED;
                ioev->order_id = order.id;
                ioev->account_id = order.account_id;
                ioev->price = order.price;
                ioev->quantity = order.quantity;
                ioev->filled_qty = 0;
                ioev->timestamp_ns = order.timestamp_ns;
                ioev->side = (uint8_t)(order.is_buy() ? 0 : 1);
                ioev->order_type = (uint8_t)(order.type == OrderType::LIMIT ? 0 : 1);
                ioev->status = (uint8_t)OrderStatus::REJECTED;
                memcpy(ioev->symbol, order.symbol, sizeof(ioev->symbol));
                strncpy(ioev->reject_reason, risk_engine_->last_reject_reason(),
                        sizeof(ioev->reject_reason) - 1);
                ioev->send_to_gateway = false;
                ring_buffer_push(&io_filled_queue_, ioev);
            } else {
                stats_.io_events_dropped++;
            }
            continue;
        }
        stats_.orders_accepted++;

        /* Stage 1c: Submit to order book */
        Order result = order_book_->add_order(order);

        /* Stage 1d: Prepare gateway serialization (CPU-only in hot path) */
        bool send_to_gw = (result.status == OrderStatus::NEW ||
                           result.status == OrderStatus::PARTIAL);
        char gw_msg[GW_MAX_MSG_SIZE];
        uint32_t gw_msg_len = 0;
        if (send_to_gw) {
            char side_ch = result.is_buy() ? 'B' : 'S';
            char type_ch = (result.type == OrderType::LIMIT) ? 'L' : 'M';
            if (gw_serialize_order(gw_msg, sizeof(gw_msg),
                                    result.id, result.symbol,
                                    side_ch, type_ch,
                                    result.price, result.quantity,
                                    result.timestamp_ns) == 0) {
                gw_msg_len = (uint32_t)strlen(gw_msg);
                stats_.orders_executed++;
            }
        }

        /* Push order result to I/O queue — the I/O thread handles all
         * journal writes, logging, and gateway dispatch. */
        IOEvent *ioev = (IOEvent *)ring_buffer_pop(&io_free_queue_);
        if (ioev) {
            ioev->type = (result.filled_qty > 0) ? IOEventType::ORDER_FILLED
                                                  : IOEventType::ORDER_ACCEPTED;
            ioev->order_id = result.id;
            ioev->account_id = result.account_id;
            ioev->price = result.price;
            ioev->quantity = result.quantity;
            ioev->filled_qty = result.filled_qty;
            ioev->timestamp_ns = result.timestamp_ns;
            ioev->side = (uint8_t)(result.is_buy() ? 0 : 1);
            ioev->order_type = (uint8_t)(result.type == OrderType::LIMIT ? 0 : 1);
            ioev->status = (uint8_t)result.status;
            memcpy(ioev->symbol, result.symbol, sizeof(ioev->symbol));
            ioev->send_to_gateway = send_to_gw;
            if (send_to_gw) {
                memcpy(ioev->gw_msg, gw_msg, gw_msg_len + 1);
                ioev->gw_msg_len = gw_msg_len;
            }
            ring_buffer_push(&io_filled_queue_, ioev);
        } else {
            stats_.io_events_dropped++;
        }

        /* Check for trades generated */
        std::vector<Trade> trades = order_book_->drain_trades();
        for (const auto &trade : trades) {
            stats_.trades_generated++;

            /* Push trade to I/O queue */
            IOEvent *tev = (IOEvent *)ring_buffer_pop(&io_free_queue_);
            if (tev) {
                tev->type = IOEventType::TRADE;
                tev->trade_id = trade.trade_id;
                tev->buy_order_id = trade.buy_order_id;
                tev->sell_order_id = trade.sell_order_id;
                tev->trade_price = trade.price;
                tev->trade_qty = trade.quantity;
                tev->trade_ts_ns = trade.timestamp_ns;
                memcpy(tev->trade_symbol, trade.symbol, sizeof(tev->trade_symbol));
                ring_buffer_push(&io_filled_queue_, tev);
            } else {
                stats_.io_events_dropped++;
            }
        }

        /* Update risk engine positions for fills */
        if (result.filled_qty > 0) {
            risk_engine_->on_order_executed(result, result.filled_qty,
                                             result.price > 0 ? result.price : mid);
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

/* ---- Stage 2: I/O Event Processing (I/O Thread) ---- */

void EventPipeline::process_io_event(const IOEvent &event) {
    switch (event.type) {
    case IOEventType::ORDER_REJECTED:
        LOG_WARN_(logger_, "Order %d REJECTED: %s",
                  event.order_id, event.reject_reason);
        if (journal_) {
            journal_log_order(journal_, JRNL_ORDER_REJECT,
                              event.order_id, event.account_id,
                              event.price, event.quantity, 0,
                              event.timestamp_ns,
                              event.side, event.order_type,
                              (uint8_t)OrderStatus::REJECTED,
                              event.symbol);
        }
        break;

    case IOEventType::ORDER_ACCEPTED:
        if (journal_) {
            journal_log_order(journal_, JRNL_ORDER_NEW,
                              event.order_id, event.account_id,
                              event.price, event.quantity,
                              event.filled_qty,
                              event.timestamp_ns,
                              event.side, event.order_type,
                              event.status,
                              event.symbol);
        }
        if (event.send_to_gateway) {
            gw_send_order(gateway_, event.gw_msg, event.gw_msg_len,
                          event.order_id);
        }
        break;

    case IOEventType::ORDER_FILLED:
        LOG_INFO_(logger_, "Order %d FILLED: %s %d@%.2f status=%d",
                  event.order_id,
                  event.side == 0 ? "BUY" : "SELL",
                  event.filled_qty, event.price,
                  (int)event.status);
        if (journal_) {
            journal_log_order(journal_, JRNL_ORDER_FILLED,
                              event.order_id, event.account_id,
                              event.price, event.quantity,
                              event.filled_qty,
                              event.timestamp_ns,
                              event.side, event.order_type,
                              event.status,
                              event.symbol);
        }
        if (event.send_to_gateway) {
            gw_send_order(gateway_, event.gw_msg, event.gw_msg_len,
                          event.order_id);
        }
        break;

    case IOEventType::TRADE:
        LOG_INFO_(logger_,
                  "TRADE id=%d %s %d@%.2f (buy=%d, sell=%d)",
                  event.trade_id, event.trade_symbol,
                  event.trade_qty, event.trade_price,
                  event.buy_order_id, event.sell_order_id);
        if (journal_) {
            journal_log_trade(journal_,
                              event.trade_id,
                              event.buy_order_id,
                              event.sell_order_id,
                              event.trade_price,
                              event.trade_qty,
                              event.trade_ts_ns,
                              event.trade_symbol);
        }
        break;
    }
}

/* ---- Consumer Thread (Stage 1: Hot Path) ---- */

void EventPipeline::consumer_loop() {
    ring_buffer_t *queue = feed_get_output_queue(feed_handler_);
    int empty_spins = 0;

    while (running_.load(std::memory_order_relaxed)) {
        feed_msg_t *msg = (feed_msg_t *)ring_buffer_pop(queue);
        if (msg) {
            empty_spins = 0;
            process_market_data(msg);
        } else {
            /* No data — progressive backoff to balance latency vs CPU usage.
             * Spin up to 1000 iterations (~1-2 µs), then yield to kernel.
             * This avoids the ~50 µs usleep() syscall penalty on empty queues
             * while still capping CPU usage when idle. */
            if (empty_spins < 1000) {
                empty_spins++;
                __builtin_ia32_pause(); /* CPU relax — ~10 cycles on x86 */
            } else {
                usleep(50);
                empty_spins = 0;
            }
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

/* ---- I/O Thread (Stage 2: Journal + Logging + Gateway) ---- */

void EventPipeline::io_loop() {
    int empty_spins = 0;

    while (running_.load(std::memory_order_relaxed)) {
        /* Pop from filled queue — only filled events arrive here */
        IOEvent *event = (IOEvent *)ring_buffer_pop(&io_filled_queue_);
        if (event) {
            empty_spins = 0;

            process_io_event(*event);

            /* Return event to the free pool for reuse by the hot path */
            ring_buffer_push(&io_free_queue_, event);
        } else {
            /* Progressive backoff: spin briefly, then yield */
            if (empty_spins < 1000) {
                empty_spins++;
                __builtin_ia32_pause();
            } else {
                usleep(50);
                empty_spins = 0;
            }

        }
    }

    /* Drain remaining filled events on stop */
    for (;;) {
        IOEvent *event = (IOEvent *)ring_buffer_pop(&io_filled_queue_);
        if (!event) break;
        process_io_event(*event);
        /* Don't return to free pool during drain — we're shutting down */
    }
}

void *EventPipeline::io_thread_func(void *arg) {
    EventPipeline *pipeline = static_cast<EventPipeline *>(arg);
    pipeline->io_loop();
    return nullptr;
}

/* ---- Start / Stop ---- */

int EventPipeline::start() {
    if (running_.load(std::memory_order_relaxed)) return 0;

    LOG_INFO_(logger_, "EventPipeline starting...");

    /* Start feed handler (generates market data) */
    if (feed_start(feed_handler_) != 0) {
        LOG_ERROR_(logger_, "Failed to start feed handler");
        return -1;
    }

    /* Start I/O thread first (must be ready to consume events) */
    running_.store(true, std::memory_order_relaxed);
    if (pthread_create(&io_thread_, nullptr, io_thread_func, this) != 0) {
        running_.store(false, std::memory_order_relaxed);
        feed_stop(feed_handler_);
        LOG_ERROR_(logger_, "Failed to create I/O thread");
        return -1;
    }

    /* Start consumer thread (hot path) */
    if (pthread_create(&consumer_thread_, nullptr,
                       consumer_thread_func, this) != 0) {
        running_.store(false, std::memory_order_relaxed);
        pthread_join(io_thread_, nullptr);
        feed_stop(feed_handler_);
        LOG_ERROR_(logger_, "Failed to create consumer thread");
        return -1;
    }

    LOG_INFO_(logger_, "EventPipeline started — 2-stage pipeline active");
    return 0;
}

void EventPipeline::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;

    LOG_INFO_(logger_, "EventPipeline stopping...");
    /* Signal threads to stop (atomic store with release to ensure visibility) */
    running_.store(false, std::memory_order_release);
    pthread_join(consumer_thread_, nullptr);

    /* Stop feed handler */
    feed_stop(feed_handler_);

    /* Wait for I/O thread to drain remaining events */
    pthread_join(io_thread_, nullptr);

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
    LOG_INFO_(logger_, "  I/O events dropped:%lu", (unsigned long)stats_.io_events_dropped);
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
