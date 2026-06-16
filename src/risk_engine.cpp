/**
 * risk_engine.cpp — Risk Engine Implementation
 */
#include "risk_engine.h"
#include <cmath>
#include <ctime>

RiskEngine::RiskEngine()
    : order_count_this_sec_(0), sec_window_start_(0), killed_(false) {}

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
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);  /* second resolution sufficient */
    now_sec = (uint64_t)ts.tv_sec;

    if (now_sec != sec_window_start_) {
        sec_window_start_ = now_sec;
        order_count_this_sec_ = 0;
    }
}

bool RiskEngine::check_order_size(const Order &order) {
    if (order.quantity > limits_.max_order_qty) {
        last_reject_ = "Order size " + std::to_string(order.quantity) +
                       " exceeds limit " + std::to_string(limits_.max_order_qty);
        return false;
    }
    if (order.quantity <= 0) {
        last_reject_ = "Order quantity must be positive";
        return false;
    }
    return true;
}

bool RiskEngine::check_position_limit(const Order &order, const std::string &sym) {
    auto it = positions_.find(sym);
    int32_t current_pos = (it != positions_.end()) ? it->second.net_position : 0;

    int32_t new_pos;
    if (order.is_buy()) {
        new_pos = current_pos + order.quantity;
    } else {
        new_pos = current_pos - order.quantity;
    }

    if (std::abs(new_pos) > limits_.max_position) {
        last_reject_ = "Position " + std::to_string(new_pos) +
                       " exceeds limit " + std::to_string(limits_.max_position) +
                       " for " + sym;
        return false;
    }
    return true;
}

bool RiskEngine::check_exposure_limit(const Order &order, const std::string &sym) {
    auto it = positions_.find(sym);
    int32_t current_exp = (it != positions_.end()) ? it->second.gross_exposure : 0;

    int32_t new_exp = current_exp + order.quantity;
    if (new_exp > limits_.max_exposure) {
        last_reject_ = "Gross exposure " + std::to_string(new_exp) +
                       " exceeds limit " + std::to_string(limits_.max_exposure) +
                       " for " + sym;
        return false;
    }
    return true;
}

bool RiskEngine::check_notional_limit(const Order &order, const std::string &sym) {
    if (order.price <= 0 && order.type == OrderType::LIMIT) {
        return true;
    }
    double price = (order.price > 0) ? order.price : 1.0;
    double notional = price * (double)order.quantity;

    auto it = positions_.find(sym);
    double current_notional = (it != positions_.end()) ? it->second.total_notional : 0.0;

    if (current_notional + notional > limits_.max_notional) {
        last_reject_ = "Notional value " + std::to_string(notional) +
                       " exceeds limit " + std::to_string(limits_.max_notional) +
                       " for " + sym;
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
        last_reject_ = "Price " + std::to_string(order.price) +
                       " deviates " + std::to_string(deviation) +
                       "% from mid " + std::to_string(mid_price) +
                       " (limit " + std::to_string(limits_.price_band_pct) + "%)";
        return false;
    }
    return true;
}

bool RiskEngine::check_rate_limit() {
    if (limits_.max_orders_per_sec <= 0) {
        return true; /* No rate limit */
    }
    if (order_count_this_sec_ >= limits_.max_orders_per_sec) {
        last_reject_ = "Rate limit exceeded: " +
                       std::to_string(order_count_this_sec_) +
                       " orders in current second";
        return false;
    }
    return true;
}

bool RiskEngine::validate(const Order &order, double mid_price) {
    last_reject_.clear();

    if (killed_) {
        last_reject_ = "Risk engine killed — all orders rejected";
        return false;
    }

    if (!limits_.enabled) {
        return true; /* Risk checks disabled */
    }

    update_rate_window();

    /* Construct symbol string once — reused by position/exposure/notional checks */
    std::string sym(order.symbol);

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

const std::string &RiskEngine::last_reject_reason() const {
    return last_reject_;
}

void RiskEngine::on_order_executed(const Order &order, int32_t filled_qty,
                                    double fill_price) {
    /* Heterogeneous lookup: find with const char* — no std::string construction.
     * If not found, emplace a new entry. */
    auto it = positions_.find(order.symbol);
    if (it == positions_.end()) {
        it = positions_.emplace(std::string(order.symbol), Position{}).first;
    }
    Position &pos = it->second;

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
    auto it = positions_.find(symbol.c_str());
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
