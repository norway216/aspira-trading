/**
 * risk_engine.cpp — Enhanced Risk Engine Implementation
 *
 * Per hft_risk_engine_design.md:
 *   §4.1 Pre-Trade Checks: size, price band, instrument allowlist, self-trade
 *   §4.2 Position Risk: net position, gross exposure, notional
 *   §4.3 Exposure Control: portfolio exposure, position % of equity
 *   §4.4 / §9 Token Bucket Rate Limiter (burst-aware)
 *   §11 SAFE MODE: auto-triggers with health metrics
 */
#include "risk_engine.h"
#include <cmath>
#include <cstdio>
#include <ctime>

/* ---- Construction ---- */

RiskEngine::RiskEngine()
    : safe_mode_(false), killed_(false),
      health_reject_rate_(0.0), health_latency_us_(0.0),
      consecutive_rejects_(0)
{
    last_reject_[0] = '\0';
    safe_mode_reason_[0] = '\0';

    /* Initialize token bucket with default limits */
    tb_init(&token_bucket_, limits_.max_orders_per_sec, limits_.burst_capacity);
}

RiskEngine::~RiskEngine() = default;

/* ---- Configuration ---- */

void RiskEngine::set_limits(const RiskLimits &limits) {
    limits_ = limits;
    /* Synchronize token bucket with new rate limits */
    tb_update(&token_bucket_, limits.max_orders_per_sec, limits.burst_capacity);
}

const RiskLimits &RiskEngine::limits() const {
    return limits_;
}

void RiskEngine::set_equity(double equity) {
    equity_ = equity;
}

void RiskEngine::allow_symbol(const char *symbol) {
    if (symbol && symbol[0]) {
        SymbolKey key(symbol);
        allowlist_[key] = true;
    }
}

void RiskEngine::deny_symbol(const char *symbol) {
    if (symbol && symbol[0]) {
        SymbolKey key(symbol);
        allowlist_.erase(key);
    }
}

void RiskEngine::clear_allowlist() {
    allowlist_.clear();
}

void RiskEngine::set_event_callback(risk_event_callback_t cb, void *ctx) {
    event_cb_ = cb;
    event_cb_ctx_ = ctx;
}

/* ---- Internal: Risk Event Emission ---- */

void RiskEngine::emit_risk_event(RiskEventType type, const char *symbol,
                                  int32_t order_id, const char *reason,
                                  double metric) {
    if (!event_cb_) return;

    RiskEvent ev;
    ev.type = type;
    ev.order_id = order_id;
    ev.metric_value = metric;

    if (symbol) {
        size_t n = strnlen(symbol, 16);
        memcpy(ev.symbol, symbol, n);
        if (n < 16) ev.symbol[n] = '\0';
    } else {
        ev.symbol[0] = '\0';
    }

    if (reason) {
        size_t n = strnlen(reason, sizeof(ev.reason) - 1);
        memcpy(ev.reason, reason, n);
        ev.reason[n] = '\0';
    } else {
        ev.reason[0] = '\0';
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ev.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    event_cb_(&ev, event_cb_ctx_);
}

/* ---- Internal Checks (in validate() order — frequency-ordered for branches) ---- */

bool RiskEngine::check_instrument_allowed(const Order &order) {
    if (allowlist_.empty()) return true; /* No allowlist = all allowed */

    SymbolKey key(order.symbol);
    auto it = allowlist_.find(key);
    if (it == allowlist_.end()) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Symbol %.16s not in instrument allowlist", order.symbol);
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, 0.0);
        return false;
    }
    return true;
}

bool RiskEngine::check_self_trade_prevention(const Order &order) {
    /* Self-trade prevention: reject if an order from the same account
     * would immediately match against the same account's resting order
     * on the opposite side at the same or better price. */
    SymbolKey sym(order.symbol);
    auto it = positions_.find(sym);
    if (it == positions_.end()) return true; /* No position = no self-trade risk */

    /* If last order on this symbol was the opposite side, and this order's
     * price crosses the book, we might self-trade. This is a simple
     * heuristic — a full implementation would check the order book directly. */
    const Position &pos = it->second;
    if (pos.last_order_id == 0) return true; /* No prior order to check */

    bool is_opposite_side = (order.is_buy() && pos.last_side == 1) ||
                            (order.is_sell() && pos.last_side == 0);
    if (!is_opposite_side) return true;

    /* Both sides have orders from the same account — potential self-trade.
     * Reject proactively. In production, this check would consult the actual
     * order book to see if the resting order still exists. */
    snprintf(last_reject_, sizeof(last_reject_),
             "Self-trade prevention: order %d on %.16s would cross own order %d",
             order.id, order.symbol, pos.last_order_id);
    emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                    order.id, last_reject_, 0.0);
    return false;
}

bool RiskEngine::check_order_size(const Order &order) {
    if (order.quantity <= 0) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Order quantity must be positive, got %d", order.quantity);
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, 0.0);
        return false;
    }
    if (order.quantity > limits_.max_order_qty) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Order size %d exceeds limit %ld",
                 order.quantity, (long)limits_.max_order_qty);
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, (double)order.quantity);
        return false;
    }
    return true;
}

bool RiskEngine::check_single_order_notional(const Order &order) {
    if (limits_.max_single_order_notional <= 0.0) return true; /* Disabled */
    if (order.price <= 0) return true; /* Market order — cannot check */

    double notional = order.price * (double)order.quantity;
    if (notional > limits_.max_single_order_notional) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Single order notional %.2f exceeds cap %.2f",
                 notional, limits_.max_single_order_notional);
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, notional);
        return false;
    }
    return true;
}

bool RiskEngine::check_position_limit(const Order &order, const SymbolKey &sym) {
    auto it = positions_.find(sym);
    int64_t current_pos = (it != positions_.end()) ? it->second.net_position : 0;

    int64_t new_pos;
    if (order.is_buy()) {
        new_pos = current_pos + (int64_t)order.quantity;
    } else {
        new_pos = current_pos - (int64_t)order.quantity;
    }

    if (new_pos > limits_.max_position || new_pos < -limits_.max_position) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Position %ld exceeds limit %ld for %.16s",
                 (long)new_pos, (long)limits_.max_position, sym.data);
        emit_risk_event(RiskEventType::LIMIT_BREACHED, sym.data,
                        order.id, last_reject_, (double)new_pos);
        return false;
    }
    return true;
}

bool RiskEngine::check_exposure_limit(const Order &order, const SymbolKey &sym) {
    auto it = positions_.find(sym);
    int64_t current_exp = (it != positions_.end()) ? it->second.gross_exposure : 0;

    int64_t new_exp = current_exp + (int64_t)order.quantity;
    if (new_exp > limits_.max_exposure) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Gross exposure %ld exceeds limit %ld for %.16s",
                 (long)new_exp, (long)limits_.max_exposure, sym.data);
        emit_risk_event(RiskEventType::LIMIT_BREACHED, sym.data,
                        order.id, last_reject_, (double)new_exp);
        return false;
    }
    return true;
}

bool RiskEngine::check_notional_limit(const Order &order, const SymbolKey &sym) {
    if (order.price <= 0 && order.type == OrderType::LIMIT) {
        return true; /* Cannot compute notional without price */
    }
    double price = (order.price > 0) ? order.price : 1.0;
    double notional = price * (double)order.quantity;

    auto it = positions_.find(sym);
    double current_notional = (it != positions_.end()) ? it->second.total_notional : 0.0;

    if (current_notional + notional > limits_.max_notional_per_symbol) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Notional %.2f exceeds limit %.2f for %.16s",
                 notional, limits_.max_notional_per_symbol, sym.data);
        emit_risk_event(RiskEventType::LIMIT_BREACHED, sym.data,
                        order.id, last_reject_, notional);
        return false;
    }
    return true;
}

bool RiskEngine::check_portfolio_exposure(const Order &order, const SymbolKey &sym,
                                           double mid_price) {
    if (limits_.max_portfolio_exposure <= 0.0) return true; /* Disabled */

    /* Sum total notional across all symbols */
    double total_notional = 0.0;
    for (const auto &kv : positions_) {
        total_notional += kv.second.total_notional;
    }

    double order_price = (order.price > 0) ? order.price : mid_price;
    if (order_price <= 0) order_price = 1.0;
    double order_notional = order_price * (double)order.quantity;

    if (total_notional + order_notional > limits_.max_portfolio_exposure) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Portfolio exposure %.2f exceeds limit %.2f "
                 "(current %.2f + order %.2f)",
                 total_notional + order_notional,
                 limits_.max_portfolio_exposure, total_notional, order_notional);
        emit_risk_event(RiskEventType::LIMIT_BREACHED, sym.data,
                        order.id, last_reject_, total_notional + order_notional);
        return false;
    }
    return true;
}

bool RiskEngine::check_position_pct_of_equity(const Order &order, const SymbolKey &sym,
                                                double mid_price) {
    if (limits_.max_position_pct <= 0.0) return true; /* Disabled */
    if (equity_ <= 0.0) return true; /* No equity configured */

    auto it = positions_.find(sym);
    double current_notional = (it != positions_.end()) ? it->second.total_notional : 0.0;

    double order_price = (order.price > 0) ? order.price : mid_price;
    if (order_price <= 0) order_price = 1.0;
    double order_notional = order_price * (double)order.quantity;

    double total_notional = current_notional + order_notional;
    double pct = total_notional / equity_;

    if (pct > limits_.max_position_pct) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Position %.2f%% of equity exceeds limit %.2f%% for %.16s",
                 pct * 100.0, limits_.max_position_pct * 100.0, sym.data);
        emit_risk_event(RiskEventType::LIMIT_BREACHED, sym.data,
                        order.id, last_reject_, pct * 100.0);
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
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, deviation);
        return false;
    }
    return true;
}

bool RiskEngine::check_rate_limit() {
    if (limits_.max_orders_per_sec <= 0) {
        return true; /* No rate limit */
    }

    /* Token bucket check — lock-free, burst-aware */
    if (!tb_allow(&token_bucket_)) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Rate limit exceeded: %d orders/sec (burst cap %d)",
                 limits_.max_orders_per_sec, limits_.burst_capacity);
        emit_risk_event(RiskEventType::ORDER_REJECTED, nullptr,
                        -1, last_reject_, 0.0);
        return false;
    }
    return true;
}

/* ---- SAFE MODE Auto-Triggers ---- */

bool RiskEngine::evaluate_safe_mode_triggers() {
    if (safe_mode_) return false; /* Already in safe mode */

    bool should_enter = false;
    char trigger_reason[256] = {0};

    /* Trigger 1: reject rate too high */
    if (limits_.safe_mode_reject_pct > 0.0 &&
        health_reject_rate_ > limits_.safe_mode_reject_pct) {
        snprintf(trigger_reason, sizeof(trigger_reason),
                 "Reject rate %.1f%% > limit %.1f%%",
                 health_reject_rate_, limits_.safe_mode_reject_pct);
        should_enter = true;
    }

    /* Trigger 2: latency too high */
    if (!should_enter && limits_.safe_mode_latency_us > 0.0 &&
        health_latency_us_ > limits_.safe_mode_latency_us) {
        snprintf(trigger_reason, sizeof(trigger_reason),
                 "Avg latency %.1f us > limit %.1f us",
                 health_latency_us_, limits_.safe_mode_latency_us);
        should_enter = true;
    }

    /* Trigger 3: N consecutive rejects */
    if (!should_enter && limits_.safe_mode_consecutive > 0 &&
        consecutive_rejects_ >= limits_.safe_mode_consecutive) {
        snprintf(trigger_reason, sizeof(trigger_reason),
                 "%d consecutive rejects (limit %d)",
                 consecutive_rejects_, limits_.safe_mode_consecutive);
        should_enter = true;
    }

    if (should_enter) {
        enter_safe_mode(trigger_reason);
        return true;
    }
    return false;
}

/* ---- Hot-Path Validation ---- */

bool RiskEngine::validate(const Order &order, double mid_price) {
    last_reject_[0] = '\0';

    /* 0. Kill switch — fastest check first */
    if (killed_) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "Risk engine killed — all orders rejected");
        emit_risk_event(RiskEventType::KILL_SWITCH, order.symbol,
                        order.id, last_reject_, 0.0);
        return false;
    }

    /* 1. SAFE MODE — reject all new orders */
    if (safe_mode_) {
        snprintf(last_reject_, sizeof(last_reject_),
                 "SAFE MODE active: %s", safe_mode_reason_);
        emit_risk_event(RiskEventType::ORDER_REJECTED, order.symbol,
                        order.id, last_reject_, 0.0);
        consecutive_rejects_++;
        evaluate_safe_mode_triggers();
        return false;
    }

    /* 2. Master enable switch */
    if (!limits_.enabled) {
        return true; /* Risk checks disabled — allow all */
    }

    /* Construct SymbolKey once — reused for all checks */
    SymbolKey sym(order.symbol);

    /* 3. Check chain — ordered by frequency: fastest/most-likely-to-fail first.
     *    Each check returns false immediately on failure (short-circuit). */

    /* 3a. Rate limit — most common rejection in HFT (token bucket, lock-free) */
    if (!check_rate_limit())
        goto reject;

    /* 3b. Order size — second most common */
    if (!check_order_size(order))
        goto reject;

    /* 3c. Instrument allowlist — fast unordered_map lookup */
    if (!check_instrument_allowed(order))
        goto reject;

    /* 3d. Single order notional cap — simple multiplication */
    if (!check_single_order_notional(order))
        goto reject;

    /* 3e. Price band — requires mid_price */
    if (!check_price_band(order, mid_price))
        goto reject;

    /* 3f. Position limit — requires hash lookup */
    if (!check_position_limit(order, sym))
        goto reject;

    /* 3g. Exposure limit */
    if (!check_exposure_limit(order, sym))
        goto reject;

    /* 3h. Notional limit */
    if (!check_notional_limit(order, sym))
        goto reject;

    /* 3i. Portfolio exposure (if enabled) */
    if (!check_portfolio_exposure(order, sym, mid_price))
        goto reject;

    /* 3j. Position % of equity (if enabled) */
    if (!check_position_pct_of_equity(order, sym, mid_price))
        goto reject;

    /* 3k. Self-trade prevention (opt-in — requires order book integration
     * for accurate same-account crossing detection. Without order book access,
     * this check produces false positives when both buy and sell orders
     * from the same strategy rest at different prices. Enable only when
     * the order book query is wired in. */
    /* DISABLED by default:
    if (!check_self_trade_prevention(order))
        goto reject;
    (void)order;  */

    /* 4. Order passed all checks — reset consecutive reject counter */
    consecutive_rejects_ = 0;
    return true;

reject:
    /* Track consecutive rejects for SAFE MODE trigger */
    consecutive_rejects_++;
    evaluate_safe_mode_triggers();
    return false;
}

const char *RiskEngine::last_reject_reason() const {
    return last_reject_;
}

/* ---- Position Management ---- */

void RiskEngine::on_order_executed(const Order &order, int32_t filled_qty,
                                    double fill_price) {
    SymbolKey sym(order.symbol);
    Position &pos = positions_[sym];

    if (order.is_buy()) {
        /* Update weighted average entry price */
        if (pos.net_position > 0 && pos.avg_entry_price > 0.0) {
            double total_cost = (double)pos.net_position * pos.avg_entry_price
                                + (double)filled_qty * fill_price;
            pos.net_position += (int64_t)filled_qty;
            pos.avg_entry_price = total_cost / (double)pos.net_position;
        } else {
            pos.net_position += (int64_t)filled_qty;
            pos.avg_entry_price = fill_price;
        }
    } else {
        pos.net_position -= (int64_t)filled_qty;
    }

    pos.gross_exposure += (int64_t)filled_qty;
    pos.total_notional += fill_price * (double)filled_qty;

    /* Track last order for self-trade prevention */
    pos.last_order_id = order.id;
    pos.last_side = (uint8_t)(order.is_buy() ? 0 : 1);
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
    safe_mode_ = false;
    killed_ = false;
    consecutive_rejects_ = 0;
    health_reject_rate_ = 0.0;
    health_latency_us_ = 0.0;
    last_reject_[0] = '\0';
    safe_mode_reason_[0] = '\0';
    tb_reset(&token_bucket_);
}

/* ---- SAFE MODE (§11) ---- */

void RiskEngine::enter_safe_mode(const char *reason) {
    if (safe_mode_) return; /* Already in safe mode */

    safe_mode_ = true;
    if (reason) {
        size_t n = strnlen(reason, sizeof(safe_mode_reason_) - 1);
        memcpy(safe_mode_reason_, reason, n);
        safe_mode_reason_[n] = '\0';
    } else {
        safe_mode_reason_[0] = '\0';
    }

    emit_risk_event(RiskEventType::SAFE_MODE_ENTERED, nullptr, -1,
                    safe_mode_reason_, 0.0);
}

void RiskEngine::exit_safe_mode() {
    if (!safe_mode_) return;

    safe_mode_ = false;
    consecutive_rejects_ = 0;
    safe_mode_reason_[0] = '\0';

    emit_risk_event(RiskEventType::SAFE_MODE_EXITED, nullptr, -1,
                    "Manual exit", 0.0);
}

bool RiskEngine::is_safe_mode() const {
    return safe_mode_;
}

const char *RiskEngine::safe_mode_reason() const {
    return safe_mode_reason_;
}

void RiskEngine::update_health_metrics(double reject_rate_pct, double avg_latency_us) {
    health_reject_rate_ = reject_rate_pct;
    health_latency_us_ = avg_latency_us;
}

/* ---- Kill Switch ---- */

void RiskEngine::kill_switch(bool enabled) {
    killed_ = enabled;
    if (enabled) {
        emit_risk_event(RiskEventType::KILL_SWITCH, nullptr, -1,
                        "Kill switch engaged", 0.0);
    }
}

bool RiskEngine::is_killed() const {
    return killed_;
}

int32_t RiskEngine::current_rate() const {
    return tb_tokens(&token_bucket_);
}
