/**
 * risk_engine.h — Pre-Trade Risk Validation Engine
 *
 * Enhanced per hft_risk_engine_design.md:
 *   §4.1 Pre-Trade Risk Checker: size, price band, instrument allowlist, self-trade
 *   §4.2 Position Risk Manager: net position, gross exposure, notional
 *   §4.3 Exposure Control: portfolio exposure, position % of equity
 *   §4.4 Rate Limiter: token bucket (burst-aware, §9)
 *   §11 SAFE MODE: auto-triggers, kill switch, risk event callbacks
 *
 * Performance (§7):
 *   - alignas(64) on hot structs to prevent false sharing
 *   - TokenBucket rate limiter (lock-free, burst-aware)
 *   - Branch-predictable check ordering
 *   - Zero heap allocation in validate()
 */
#ifndef RISK_ENGINE_H
#define RISK_ENGINE_H

#include "order.h"
#include "rate_limiter.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

/* ---- Symbol Key (zero-allocation lookup) ---- */

struct SymbolKey {
    char data[16];

    SymbolKey() { memset(data, 0, sizeof(data)); }
    explicit SymbolKey(const char *src) {
        size_t n = strnlen(src, 16);
        memcpy(data, src, n);
        if (n < 16) memset(data + n, 0, 16 - n);
    }
    explicit SymbolKey(const std::string &src) {
        size_t n = src.size();
        memcpy(data, src.c_str(), n < 16 ? n : 16);
        if (n < 16) memset(data + n, 0, 16 - n);
    }

    bool operator==(const SymbolKey &other) const {
        return memcmp(data, other.data, 16) == 0;
    }
};

struct SymbolKeyHash {
    size_t operator()(const SymbolKey &key) const {
        size_t h = 14695981039346656037ULL;
        for (int i = 0; i < 16 && key.data[i]; i++) {
            h ^= (unsigned char)key.data[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
};

/* ---- Cache-Aligned Data Structures (§7.1) ---- */

/**
 * Per-symbol position tracking.
 * alignas(64) — cache-line aligned to prevent false sharing.
 * Upgraded to int64_t for institutional-scale positions.
 */
struct alignas(64) Position {
    int64_t net_position     = 0;   /* +long, -short */
    int64_t gross_exposure   = 0;   /* total absolute quantity traded */
    double  total_notional   = 0;   /* total notional traded */
    double  avg_entry_price  = 0;   /* weighted average entry price */
    int32_t last_order_id    = 0;   /* self-trade prevention: last order on this symbol */
    uint8_t last_side        = 0;   /* self-trade prevention: side of last order */
    char    _pad[27];               /* pad to 64 bytes */
};

static_assert(sizeof(Position) % 64 == 0, "Position must be cache-line aligned");

/**
 * Configurable risk limits.
 * alignas(64) — frequently read in hot path, own cache line.
 */
struct alignas(64) RiskLimits {
    /* Per-order limits (§4.1) */
    int64_t max_order_qty             = 10000;
    double  max_single_order_notional = 0.0;    /* 0 = disabled */

    /* Position limits (§4.2) */
    int64_t max_position              = 100000;
    int64_t max_exposure              = 500000;
    double  max_notional_per_symbol   = 10000000.0;

    /* Portfolio limits (§4.3) */
    double  max_portfolio_exposure    = 0.0;     /* 0 = disabled */
    double  max_position_pct          = 0.0;     /* % of equity (0 = disabled) */

    /* Price bands (§4.1) */
    double  price_band_pct            = 5.0;

    /* Rate limiting (§9 Token Bucket) */
    int32_t max_orders_per_sec        = 1000;    /* sustained rate */
    int32_t burst_capacity            = 2000;    /* burst tokens */

    /* SAFE MODE auto-triggers (§11) */
    double  safe_mode_reject_pct      = 50.0;    /* if reject rate > this % */
    double  safe_mode_latency_us      = 1000.0;  /* if avg latency > this us */
    int32_t safe_mode_consecutive     = 0;       /* N consecutive rejects (0=off) */

    /* Master switch */
    bool    enabled                   = true;
    char    _pad[35];                            /* pad to 128 bytes (2 cache lines) */
};

static_assert(sizeof(RiskLimits) % 64 == 0, "RiskLimits must be cache-line aligned");

/* ---- Risk Event Callback (§11) ---- */

enum class RiskEventType : uint8_t {
    ORDER_REJECTED    = 0,
    SAFE_MODE_ENTERED = 1,
    SAFE_MODE_EXITED  = 2,
    LIMIT_BREACHED    = 3,
    KILL_SWITCH       = 4,
};

struct RiskEvent {
    RiskEventType type;
    char          symbol[16];
    int32_t       order_id;
    char          reason[256];
    uint64_t      timestamp_ns;
    double        metric_value;
};

typedef void (*risk_event_callback_t)(const RiskEvent *event, void *ctx);

/* ---- Risk Engine ---- */

class RiskEngine {
public:
    RiskEngine();
    ~RiskEngine();

    /* ---- Configuration ---- */

    void set_limits(const RiskLimits &limits);
    const RiskLimits &limits() const;

    /** Set total account equity for %-based position limits. */
    void set_equity(double equity);

    /** Instrument allowlist: if non-empty, only listed symbols are accepted. */
    void allow_symbol(const char *symbol);
    void deny_symbol(const char *symbol);
    void clear_allowlist();

    /** Register a callback for risk events. Keep it fast — called from hot path. */
    void set_event_callback(risk_event_callback_t cb, void *ctx);

    /* ---- Hot-Path Validation ---- */

    bool validate(const Order &order, double mid_price = 0.0);
    const char *last_reject_reason() const;

    /* ---- Position Management ---- */

    void on_order_executed(const Order &order, int32_t filled_qty, double fill_price);
    void on_order_cancelled(const Order &order);
    Position get_position(const std::string &symbol) const;
    Position get_position(const char *symbol) const;
    void reset();

    /* ---- SAFE MODE (§11) ---- */

    void enter_safe_mode(const char *reason);
    void exit_safe_mode();
    bool is_safe_mode() const;
    const char *safe_mode_reason() const;

    /** Update health metrics for auto SAFE MODE triggers. Called periodically. */
    void update_health_metrics(double reject_rate_pct, double avg_latency_us);

    /* ---- Kill Switch ---- */

    void kill_switch(bool enabled);
    bool is_killed() const;
    int32_t current_rate() const;

private:
    /* Cache-aligned limits (read-mostly in hot path) */
    RiskLimits limits_;

    /* Token bucket rate limiter (lock-free) */
    TokenBucket token_bucket_;

    /* Reject reason — fixed buffer, no heap allocation */
    char last_reject_[256];
    char safe_mode_reason_[256];

    /* Per-symbol positions */
    std::unordered_map<SymbolKey, Position, SymbolKeyHash> positions_;

    /* Instrument allowlist (empty = all allowed) */
    std::unordered_map<SymbolKey, bool, SymbolKeyHash> allowlist_;

    /* Risk event callback */
    risk_event_callback_t event_cb_ = nullptr;
    void *event_cb_ctx_ = nullptr;

    /* Equity for %-based limits */
    double equity_ = 0.0;

    /* SAFE MODE state */
    bool safe_mode_ = false;
    bool killed_ = false;

    /* Health metrics for auto-triggers */
    double health_reject_rate_ = 0.0;
    double health_latency_us_ = 0.0;
    int32_t consecutive_rejects_ = 0;

    /* ---- Internal Checks ---- */

    bool check_instrument_allowed(const Order &order);
    bool check_self_trade_prevention(const Order &order);
    bool check_order_size(const Order &order);
    bool check_single_order_notional(const Order &order);
    bool check_position_limit(const Order &order, const SymbolKey &sym);
    bool check_exposure_limit(const Order &order, const SymbolKey &sym);
    bool check_notional_limit(const Order &order, const SymbolKey &sym);
    bool check_portfolio_exposure(const Order &order, const SymbolKey &sym, double mid_price);
    bool check_position_pct_of_equity(const Order &order, const SymbolKey &sym, double mid_price);
    bool check_price_band(const Order &order, double mid_price);
    bool check_rate_limit();

    /** Evaluate whether auto-triggers should enter SAFE MODE */
    bool evaluate_safe_mode_triggers();

    /** Emit a risk event through the callback */
    void emit_risk_event(RiskEventType type, const char *symbol,
                         int32_t order_id, const char *reason, double metric);
};

#endif /* RISK_ENGINE_H */
