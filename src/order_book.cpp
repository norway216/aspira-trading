/**
 * order_book.cpp — Price-Time Priority Order Book Implementation
 */
#include "order_book.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>

/* ---- Construction ---- */

OrderBook::OrderBook(const std::string &symbol)
    : symbol_(symbol), next_trade_id_(1) {}

OrderBook::~OrderBook() = default;

/* ---- Internal helpers ---- */

void OrderBook::generate_trade(const Order &buy, const Order &sell,
                                int32_t qty, double price) {
    Trade t;
    t.trade_id   = next_trade_id_++;
    t.buy_order_id  = buy.id;
    t.sell_order_id = sell.id;
    t.price       = price;
    t.quantity    = qty;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    t.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Copy symbol with memcpy — faster than char-by-char loop */
    size_t sym_len = symbol_.size();
    memcpy(t.symbol, symbol_.c_str(), sym_len < sizeof(t.symbol) ? sym_len : sizeof(t.symbol));
    if (sym_len < sizeof(t.symbol)) {
        t.symbol[sym_len] = '\0';
    }

    trades_.push_back(t);
}

void OrderBook::remove_from_level(OrderList &level_list,
                                   OrderList::iterator it,
                                   int32_t order_id) {
    level_list.erase(it);
    order_index_.erase(order_id);
}

/* ---- Limit Order Matching ---- */

Order OrderBook::match_limit_order(Order &order) {
    if (order.is_buy()) {
        /* Buy order: match against asks (lowest first) */
        auto ask_it = asks_.begin();
        while (ask_it != asks_.end() && order.remaining() > 0) {
            double ask_price = ask_it->first;

            /* Only match if buy price >= ask price (can cross) */
            if (order.type == OrderType::LIMIT && order.price < ask_price) {
                break; /* Cannot match — price too low */
            }

            OrderList &level = ask_it->second;
            auto entry_it = level.begin();
            while (entry_it != level.end() && order.remaining() > 0) {
                Order &resting = entry_it->order;

                int32_t match_qty = std::min(order.remaining(), resting.remaining());
                double match_price = (order.type == OrderType::MARKET)
                                         ? ask_price
                                         : ask_price;

                /* Update resting order */
                resting.filled_qty += match_qty;

                /* Update incoming order */
                order.filled_qty += match_qty;

                /* Generate trade at the resting order's price */
                generate_trade(order, resting, match_qty, match_price);

                if (resting.is_filled()) {
                    order_index_.erase(resting.id);
                    entry_it = level.erase(entry_it);
                } else {
                    resting.status = OrderStatus::PARTIAL;
                    ++entry_it;
                }
            }

            if (level.empty()) {
                ask_it = asks_.erase(ask_it);
            } else {
                ++ask_it;
            }
        }
    } else {
        /* Sell order: match against bids (highest first) */
        auto bid_it = bids_.begin();
        while (bid_it != bids_.end() && order.remaining() > 0) {
            double bid_price = bid_it->first;

            if (order.type == OrderType::LIMIT && order.price > bid_price) {
                break; /* Cannot match — price too high */
            }

            OrderList &level = bid_it->second;
            auto entry_it = level.begin();
            while (entry_it != level.end() && order.remaining() > 0) {
                Order &resting = entry_it->order;

                int32_t match_qty = std::min(order.remaining(), resting.remaining());
                double match_price = bid_price;

                resting.filled_qty += match_qty;
                order.filled_qty += match_qty;

                /* Note: generate_trade params are (buy, sell, qty, price)
                 * Here the resting order is the BUY, incoming is the SELL */
                generate_trade(resting, order, match_qty, match_price);

                if (resting.is_filled()) {
                    order_index_.erase(resting.id);
                    entry_it = level.erase(entry_it);
                } else {
                    resting.status = OrderStatus::PARTIAL;
                    ++entry_it;
                }
            }

            if (level.empty()) {
                bid_it = bids_.erase(bid_it);
            } else {
                ++bid_it;
            }
        }
    }

    /* Update status */
    if (order.is_filled()) {
        order.status = OrderStatus::FILLED;
    } else if (order.filled_qty > 0) {
        order.status = OrderStatus::PARTIAL;
    }

    return order;
}

/* ---- Market Order Matching ---- */

Order OrderBook::match_market_order(Order &order) {
    order.price = 0.0; /* Market orders have no limit price */
    return match_limit_order(order);
}

/* ---- Public API ---- */

Order OrderBook::add_order(const Order &order) {
    Order result = order;

    /* First, try to match immediately */
    result = match_limit_order(result);

    /* If not fully filled and it's a limit order, add to book */
    if (!result.is_filled() && result.type == OrderType::LIMIT) {
        OrderEntry entry;
        entry.order = result;
        entry.order.status = OrderStatus::NEW;

        OrderList *level_list = nullptr;

        if (result.is_buy()) {
            level_list = &bids_[result.price];
        } else {
            level_list = &asks_[result.price];
        }

        level_list->push_back(entry);
        auto it = std::prev(level_list->end());
        it->list_iterator = nullptr; /* We don't need to store the iterator for now */

        /* Index the order for O(1) lookup */
        order_index_[result.id] = &(*it);

        /* If the order was partially filled during matching, update status */
        if (result.filled_qty > 0) {
            result.status = OrderStatus::PARTIAL;
            /* Update the book copy's status too */
            it->order.status = OrderStatus::PARTIAL;
        }
    }

    return result;
}

Order OrderBook::add_market_order(const Order &order) {
    Order result = order;
    result.type = OrderType::MARKET;
    result = match_market_order(result);
    return result;
}

bool OrderBook::cancel_order(int32_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return false;
    }

    OrderEntry *entry = it->second;
    entry->order.status = OrderStatus::CANCELLED;

    /* Find and remove from price level */
    if (entry->order.is_buy()) {
        auto level_it = bids_.find(entry->order.price);
        if (level_it != bids_.end()) {
            OrderList &level = level_it->second;
            for (auto list_it = level.begin(); list_it != level.end(); ++list_it) {
                if (list_it->order.id == order_id) {
                    level.erase(list_it);
                    break;
                }
            }
            if (level.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(entry->order.price);
        if (level_it != asks_.end()) {
            OrderList &level = level_it->second;
            for (auto list_it = level.begin(); list_it != level.end(); ++list_it) {
                if (list_it->order.id == order_id) {
                    level.erase(list_it);
                    break;
                }
            }
            if (level.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    order_index_.erase(it);
    return true;
}

Order OrderBook::modify_order(int32_t order_id, double new_price, int32_t new_qty) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        Order empty;
        empty.id = INVALID_ORDER_ID;
        empty.status = OrderStatus::REJECTED;
        return empty;
    }

    OrderEntry *entry = it->second;
    Order modified = entry->order;

    /* Cancel existing order */
    cancel_order(order_id);

    /* Re-insert with new price/qty (loses time priority) */
    modified.price = new_price;
    modified.quantity = new_qty;
    modified.filled_qty = 0;
    modified.status = OrderStatus::NEW;

    return add_order(modified);
}

/* ---- Query API ---- */

double OrderBook::best_bid() const {
    if (bids_.empty()) return 0.0;
    return bids_.begin()->first;
}

double OrderBook::best_ask() const {
    if (asks_.empty()) return 0.0;
    return asks_.begin()->first;
}

int32_t OrderBook::bid_size() const {
    if (bids_.empty()) return 0;
    int32_t total = 0;
    for (const auto &entry : bids_.begin()->second) {
        total += entry.order.remaining();
    }
    return total;
}

int32_t OrderBook::ask_size() const {
    if (asks_.empty()) return 0;
    int32_t total = 0;
    for (const auto &entry : asks_.begin()->second) {
        total += entry.order.remaining();
    }
    return total;
}

double OrderBook::mid_price() const {
    double bb = best_bid();
    double ba = best_ask();
    if (bb == 0.0 || ba == 0.0) return 0.0;
    return (bb + ba) / 2.0;
}

double OrderBook::spread() const {
    double bb = best_bid();
    double ba = best_ask();
    if (bb == 0.0 || ba == 0.0) return 0.0;
    return ba - bb;
}

size_t OrderBook::bid_levels() const {
    return bids_.size();
}

size_t OrderBook::ask_levels() const {
    return asks_.size();
}

size_t OrderBook::order_count() const {
    return order_index_.size();
}

const std::string &OrderBook::symbol() const {
    return symbol_;
}

std::vector<Trade> OrderBook::drain_trades() {
    std::vector<Trade> result;
    result.swap(trades_);
    return result;
}

const std::vector<Trade> &OrderBook::trades() const {
    return trades_;
}

size_t OrderBook::trade_count() const {
    return trades_.size();
}

std::vector<OrderBook::PriceLevel>
OrderBook::get_bid_levels(size_t depth) const {
    std::vector<PriceLevel> result;
    size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < depth; ++it, ++i) {
        int32_t total = 0;
        for (const auto &entry : it->second) {
            total += entry.order.remaining();
        }
        result.emplace_back(it->first, total);
    }
    return result;
}

std::vector<OrderBook::PriceLevel>
OrderBook::get_ask_levels(size_t depth) const {
    std::vector<PriceLevel> result;
    size_t i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < depth; ++it, ++i) {
        int32_t total = 0;
        for (const auto &entry : it->second) {
            total += entry.order.remaining();
        }
        result.emplace_back(it->first, total);
    }
    return result;
}

void OrderBook::update_level(Side side, double price, int32_t size) {
    if (size == 0) {
        /* Remove the level */
        if (side == Side::BUY) {
            auto it = bids_.find(price);
            if (it != bids_.end()) {
                for (auto &entry : it->second) {
                    order_index_.erase(entry.order.id);
                }
                bids_.erase(it);
            }
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end()) {
                for (auto &entry : it->second) {
                    order_index_.erase(entry.order.id);
                }
                asks_.erase(it);
            }
        }
    }
    /* Note: for size > 0 we would need to create synthetic orders.
     * This is a simplified model — in production, you'd maintain a separate
     * market-data-driven order book, not add/remove individual orders. */
}
