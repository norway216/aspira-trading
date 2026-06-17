/**
 * risk_engine.cpp — Risk Engine Implementation
 *
 * All hot-path validation uses fixed char buffers (snprintf) instead of
 * std::to_string/std::string concatenation — zero heap allocation.
 * SymbolKey enables positions_ lookup without constructing std::string.
 */
#include "risk_engine.h"
#include <cmath>
#include <cstdio>
#include <ctime>

RiskEngine::RiskEngine()
    : order_count_this_sec_(0), sec_window_start_(0), killed_(false)
{
    last_reject_[0] = '\0';
}

RiskEngine::~RiskEngine() = default;

void RiskEngine::set_limits(const RiskLimits &limits) {
    limits_ = limits;
}

const RiskLimits &RiskEngine::limits() const {
    return limits_;
}

void RiskEngine::update_rate_window() {
    uint64_t now_sec;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    now_sec = (uint64_t)ts.tv_sec;

    if (now_sec != sec_window_start_) {
        sec_window_start_ = now_sec;
        order_count_this_sec_ = 0;
    }
}

bool RiskEngine::check_order_size(const Order &order) {
    if (order.quantity > limits_.max_order_qty) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Order size %d exceeds limit %d",
                 order.quantity, limits_.max_order_qty);
        return false;
    }
    if (order.quantity <= 0) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Order quantity must be positive");
        return false;
    }
    return true;
}

bool RiskEngine::check_position_limit(const Order &order, const SymbolKey &sym) {
    auto it = positions_.find(sym);
    int32_t current_pos = (it != positions_.end()) ? it->second.net_position : 0;

    int32_t new_pos;
    if (order.is_buy()) {
        new_pos = current_pos + order.quantity;
    } else {
        new_pos = current_pos - order.quantity;
    }

    if (std::abs(new_pos) > limits_.max_position) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Position %d exceeds limit %d for %.16s",
                 new_pos, limits_.max_position, sym.data);
        return false;
    }
    return true;
}

bool RiskEngine::check_exposure_limit(const Order &order, const SymbolKey &sym) {
    auto it = positions_.find(sym);
    int32_t current_exp = (it != positions_.end()) ? it->second.gross_exposure : 0;

    int32_t new_exp = current_exp + order.quantity;
    if (new_exp > limits_.max_exposure) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Gross exposure %d exceeds limit %d for %.16s",
                 new_exp, limits_.max_exposure, sym.data);
        return false;
    }
    return true;
}

bool RiskEngine::check_notional_limit(const Order &order, const SymbolKey &sym) {
    if (order.price <= 0 && order.type == OrderType::LIMIT) {
        return true;
    }
    double price = (order.price > 0) ? order.price : 1.0;
    double notional = price * (double)order.quantity;

    auto it = positions_.find(sym);
    double current_notional = (it != positions_.end()) ? it->second.total_notional : 0.0;

    if (current_notional + notional > limits_.max_notional) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Notional value %.2f exceeds limit %.2f for %.16s",
                 notional, limits_.max_notional, sym.data);
        return false;
    }
    return true;
}

bool RiskEngine::check_price_band(const Order &order, double mid_price) {
    if (mid_price <= 0 || order.price <= 0) {
        return true; /* Cannot validate without reference price */
    }
    if (order.type == OrderType::MARKET) {
        return true; /* Market orders have no limit price */
    }

    double deviation = std::abs(order.price - mid_price) / mid_price * 100.0;
    if (deviation > limits_.price_band_pct) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Price %.4f deviates %.2f%% from mid %.4f (limit %.2f%%)",
                 order.price, deviation, mid_price, limits_.price_band_pct);
        return false;
    }
    return true;
}

bool RiskEngine::check_rate_limit() {
    if (limits_.max_orders_per_sec <= 0) {
        return true; /* No rate limit */
    }
    if (order_count_this_sec_ >= limits_.max_orders_per_sec) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Rate limit exceeded: %d orders in current second",
                 order_count_this_sec_);
        return false;
    }
    return true;
}

bool RiskEngine::validate(const Order &order, double mid_price) {
    last_reject_[0] = '\0';

    if (killed_) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Risk engine killed — all orders rejected");
        return false;
    }

    if (!limits_.enabled) {
        return true; /* Risk checks disabled */
    }

    update_rate_window();

    /* Construct SymbolKey once on the stack — zero heap allocation.
     * This single key is reused for all position/exposure/notional checks. */
    SymbolKey sym(order.symbol);

    /* Run all checks — stop at first failure */
    if (!check_rate_limit())               return false;
    if (!check_order_size(order))          return false;
    if (!check_position_limit(order, sym)) return false;
    if (!check_exposure_limit(order, sym)) return false;
    if (!check_notional_limit(order, sym)) return false;
    if (!check_price_band(order, mid_price)) return false;

    /* Track rate */
    order_count_this_sec_++;

    return true;
}

const char *RiskEngine::last_reject_reason() const {
    return last_reject_;
}

void RiskEngine::on_order_executed(const Order &order, int32_t filled_qty,
                                    double fill_price) {
    /* SymbolKey from order.symbol — stack copy, no allocation */
    SymbolKey sym(order.symbol);
    Position &pos = positions_[sym];

    if (order.is_buy()) {
        pos.net_position += filled_qty;
    } else {
        pos.net_position -= filled_qty;
    }
    pos.gross_exposure += filled_qty;
    pos.total_notional += fill_price * (double)filled_qty;
}

void RiskEngine::on_order_cancelled(const Order & /*order*/) {
    /* No position change for cancelled orders */
}

Position RiskEngine::get_position(const std::string &symbol) const {
    SymbolKey key(symbol);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }
    return Position{};
}

Position RiskEngine::get_position(const char *symbol) const {
    SymbolKey key(symbol);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }
    return Position{};
}

void RiskEngine::reset() {
    positions_.clear();
    order_count_this_sec_ = 0;
    sec_window_start_ = 0;
    killed_ = false;
}

void RiskEngine::kill_switch(bool enabled) {
    killed_ = enabled;
}

bool RiskEngine::is_killed() const {
    return killed_;
}

int32_t RiskEngine::current_rate() const {
    return order_count_this_sec_;
}
