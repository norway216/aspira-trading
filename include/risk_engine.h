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
 */
#ifndef RISK_ENGINE_H
#define RISK_ENGINE_H

#include "order.h"
#include <cstdint>
#include <string>
#include <unordered_map>

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
     */
    const std::string &last_reject_reason() const;

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
    std::string last_reject_;
    std::unordered_map<std::string, Position> positions_;

    /* Rate limiting */
    int32_t order_count_this_sec_;
    uint64_t sec_window_start_;

    bool killed_;

    /* Internal checks — sym is pre-constructed once in validate() */
    bool check_order_size(const Order &order);
    bool check_position_limit(const Order &order, const std::string &sym);
    bool check_exposure_limit(const Order &order, const std::string &sym);
    bool check_notional_limit(const Order &order, const std::string &sym);
    bool check_price_band(const Order &order, double mid_price);
    bool check_rate_limit();
    void update_rate_window();
};

#endif /* RISK_ENGINE_H */
