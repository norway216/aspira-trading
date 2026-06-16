/**
 * order.h — Core Order and Trade Data Structures
 */
#ifndef ORDER_H
#define ORDER_H

#include <cstdint>

/* Maximum order ID (used as sentinel) */
constexpr int32_t INVALID_ORDER_ID = -1;

/* Side */
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

/* Order types */
enum class OrderType : uint8_t {
    LIMIT  = 0,   /* Resting order at a specific price */
    MARKET = 1    /* Execute immediately at best available price */
};

/* Order status */
enum class OrderStatus : uint8_t {
    NEW        = 0,
    PARTIAL    = 1,   /* Partially filled */
    FILLED     = 2,
    CANCELLED  = 3,
    REJECTED   = 4
};

/**
 * An order in the system.
 * Keep the struct cache-line friendly (64 bytes).
 */
struct Order {
    int32_t  id;           /* Unique order ID */
    int32_t  account_id;   /* Account / trader ID */
    double   price;        /* Limit price (0 for market orders) */
    int32_t  quantity;      /* Original quantity */
    int32_t  filled_qty;    /* Quantity already filled */
    uint64_t timestamp_ns;  /* Order arrival time */
    Side     side;
    OrderType type;
    OrderStatus status;
    char     symbol[16];   /* Instrument symbol */
    char     _pad[3];      /* Padding to align to 64 bytes total */

    Order() : id(INVALID_ORDER_ID), account_id(0), price(0.0),
              quantity(0), filled_qty(0), timestamp_ns(0),
              side(Side::BUY), type(OrderType::LIMIT),
              status(OrderStatus::NEW) {
        symbol[0] = '\0';
    }

    int32_t remaining() const { return quantity - filled_qty; }
    bool is_filled() const { return filled_qty >= quantity; }
    bool is_buy() const { return side == Side::BUY; }
    bool is_sell() const { return side == Side::SELL; }
};

static_assert(sizeof(Order) <= 64, "Order must fit in a cache line");

/**
 * A trade (match) between two orders.
 */
struct Trade {
    int32_t  trade_id;
    int32_t  buy_order_id;
    int32_t  sell_order_id;
    double   price;
    int32_t  quantity;
    uint64_t timestamp_ns;
    char     symbol[16];
    char     _pad[4];
};

/**
 * Market data event (received from feed).
 */
struct MarketDataEvent {
    uint64_t timestamp_ns;
    double   bid_price;
    double   ask_price;
    int32_t  bid_size;
    int32_t  ask_size;
    double   last_price;
    int32_t  last_size;
    int32_t  volume;
    char     symbol[16];
    char     _pad[4];
};

#endif /* ORDER_H */
