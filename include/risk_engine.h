/**
 * risk_engine.h — Pre-Trade Risk Validation Engine
 *
 * Checks every order before it reaches the execution gateway:
 *   - Maximum order size
 *   - Maximum position (net exposure per symbol)
 *   - Price band (reject orders too far from market)
 *   - Maximum order rate (throttling)
 *   - Maximum notional value
 *
 * All checks are in the critical path — kept simple and branch-predictable.
 * Zero heap allocation in validate() — SymbolKey avoids std::string construction,
 * and last_reject_ uses a fixed char buffer instead of std::string.
 */
#ifndef RISK_ENGINE_H
#define RISK_ENGINE_H

#include "order.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

/**
 * Stack-allocated symbol key for O(1) unordered_map lookup without
 * std::string heap allocation. Wraps the 16-byte symbol char array
 * from Order/feed_msg_t directly — construction is a simple memcpy.
 */
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

/** FNV-1a hash for SymbolKey — fast, well-distributed, no heap allocation */
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

/**
 * Per-symbol position tracking.
 */
struct Position {
    int32_t net_position = 0;   /* +long, -short */
    int32_t gross_exposure = 0; /* total absolute quantity traded */
    double  total_notional = 0; /* total notional traded */
};

/**
 * Configurable risk limits.
 */
struct RiskLimits {
    int32_t max_order_qty    = 10000;   /* Max single order quantity */
    int32_t max_position     = 100000;  /* Max net position (abs) */
    int32_t max_exposure     = 500000;  /* Max gross exposure */
    double  max_notional     = 10000000.0; /* Max notional value */
    double  price_band_pct   = 5.0;     /* Max % away from mid price */
    int32_t max_orders_per_sec = 1000;  /* Rate limit */
    bool    enabled          = true;    /* Master kill switch */
};

/**
 * Risk Engine: validates orders against configurable limits.
 * Zero heap allocation in the hot path (validate).
 */
class RiskEngine {
public:
    RiskEngine();
    ~RiskEngine();

    /**
     * Set risk limits.
     */
    void set_limits(const RiskLimits &limits);

    /**
     * Get current risk limits.
     */
    const RiskLimits &limits() const;

    /**
     * Validate an order against all risk checks.
     * @param order     The order to validate
     * @param mid_price Current mid-market price (0 if unknown)
     * @return true if the order passes all checks
     */
    bool validate(const Order &order, double mid_price = 0.0);

    /**
     * Get the reject reason for the last failed validation.
     * Returns pointer to internal buffer — valid until next validate() call.
     */
    const char *last_reject_reason() const;

    /**
     * Acknowledge that an order was executed (update positions).
     */
    void on_order_executed(const Order &order, int32_t filled_qty, double fill_price);

    /**
     * Acknowledge that an order was cancelled/rejected (no position change).
     */
    void on_order_cancelled(const Order &order);

    /**
     * Get current position for a symbol.
     */
    Position get_position(const std::string &symbol) const;
    Position get_position(const char *symbol) const;

    /**
     * Reset all positions and counters.
     */
    void reset();

    /**
     * Master kill switch — reject all orders.
     */
    void kill_switch(bool enabled);
    bool is_killed() const;

    /**
     * Get orders validated in the current second window (rate limiting).
     */
    int32_t current_rate() const;

private:
    RiskLimits limits_;
    char last_reject_[256];  /* Fixed buffer — no heap allocation in hot path */
    std::unordered_map<SymbolKey, Position, SymbolKeyHash> positions_;

    /* Rate limiting */
    int32_t order_count_this_sec_;
    uint64_t sec_window_start_;

    bool killed_;

    /* Internal checks */
    bool check_order_size(const Order &order);
    bool check_position_limit(const Order &order, const SymbolKey &sym);
    bool check_exposure_limit(const Order &order, const SymbolKey &sym);
    bool check_notional_limit(const Order &order, const SymbolKey &sym);
    bool check_price_band(const Order &order, double mid_price);
    bool check_rate_limit();
    void update_rate_window();
};

#endif /* RISK_ENGINE_H */
