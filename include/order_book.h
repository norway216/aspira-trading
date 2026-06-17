/**
 * order_book.h — Price-Time Priority Order Book Engine
 *
 * Maintains bid and ask levels. Supports:
 *   - Limit order insertion
 *   - Market order execution
 *   - Order cancellation
 *   - Price-time priority matching
 *   - Trade generation
 *
 * Uses std::map for price levels (O(log N) lookup).
 * Each price level maintains a FIFO queue of orders (time priority).
 */
#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H

#include "order.h"
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * An entry in a price-level queue.
 */
struct OrderEntry {
    Order order;
};

/**
 * Limit-order book with price-time priority.
 */
class OrderBook {
public:
    explicit OrderBook(const std::string &symbol);
    ~OrderBook();

    /* ---- Order Operations ---- */

    /**
     * Add a limit order. If it crosses the book (can be immediately matched),
     * trades are generated and the order is filled/partial-filled.
     * Returns the order with updated status and filled_qty.
     */
    Order add_order(const Order &order);

    /**
     * Add a market order. Matches against resting orders immediately.
     * Returns the order with filled_qty set (may be partial if insufficient liquidity).
     */
    Order add_market_order(const Order &order);

    /**
     * Cancel an order by ID. O(1) per order.
     * Returns true if cancelled, false if not found / already filled.
     */
    bool cancel_order(int32_t order_id);

    /**
     * Modify an existing order (price and/or quantity).
     * Internally: cancel + re-insert (loses time priority).
     * Returns the modified order.
     */
    Order modify_order(int32_t order_id, double new_price, int32_t new_qty);

    /* ---- Query Operations ---- */

    /** Best bid price (0 if no bids) */
    double best_bid() const;

    /** Best ask price (0 if no asks) */
    double best_ask() const;

    /** Total quantity at best bid */
    int32_t bid_size() const;

    /** Total quantity at best ask */
    int32_t ask_size() const;

    /** Mid price (0 if one side is empty) */
    double mid_price() const;

    /** Spread (0 if one side is empty) */
    double spread() const;

    /** Number of bid price levels */
    size_t bid_levels() const;

    /** Number of ask price levels */
    size_t ask_levels() const;

    /** Total number of resting orders */
    size_t order_count() const;

    /** Get the symbol */
    const std::string &symbol() const;

    /** Get all recent trades since last call (clears internal trade buffer) */
    std::vector<Trade> drain_trades();

    /** Get recent trades without clearing */
    const std::vector<Trade> &trades() const;

    /** Get the current trade count */
    size_t trade_count() const;

    /** Get all bid levels */
    using PriceLevel = std::pair<double, int32_t>;
    std::vector<PriceLevel> get_bid_levels(size_t depth = 10) const;
    std::vector<PriceLevel> get_ask_levels(size_t depth = 10) const;

    /* ---- Depth-of-book update ---- */

    /** Apply a market data update (replace a price level) */
    void update_level(Side side, double price, int32_t size);

private:
    /* Price → list of orders at that price (time priority).
     * std::map is cache-unfriendly but offers ordered traversal for price levels.
     * For HFT with few price levels (<500), this is acceptable — the O(log N)
     * lookup is dwarfed by matching logic. For deeper books, consider boost::flat_map. */
    using OrderList = std::list<OrderEntry>;
    using BidMap = std::map<double, OrderList, std::greater<double>>;
    using AskMap = std::map<double, OrderList, std::less<double>>;

    std::string symbol_;
    BidMap bids_;
    AskMap asks_;

    /* Double-buffered trade storage: active_trades_ points to whichever buffer
     * is currently receiving new trades. drain_trades() swaps the pointer and
     * returns the old buffer contents — zero allocation in generate_trade(). */
    std::vector<Trade> trades_buf_a_;
    std::vector<Trade> trades_buf_b_;
    std::vector<Trade> *active_trades_;

    /* Order ID → list iterator for O(1) lookup AND O(1) cancellation.
     * std::unordered_map provides true O(1) average lookup (vs std::map's O(log N)).
     * The stored iterator allows direct list erasure without linear scanning. */
    std::unordered_map<int32_t, OrderList::iterator> order_index_;

    int32_t next_trade_id_;

    /* Internal helpers */
    Order match_limit_order(Order &order);
    Order match_market_order(Order &order);
    void generate_trade(const Order &buy, const Order &sell, int32_t qty, double price);
};

#endif /* ORDER_BOOK_H */
