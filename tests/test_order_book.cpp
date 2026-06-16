/**
 * test_order_book.cpp — Unit tests for order book engine
 */
#include "../include/order_book.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static int next_id = 1000;

static Order make_order(Side side, OrderType type, double price, int32_t qty) {
    Order o;
    o.id = next_id++;
    o.account_id = 1;
    o.price = price;
    o.quantity = qty;
    o.side = side;
    o.type = type;
    strncpy(o.symbol, "AAPL", sizeof(o.symbol) - 1);
    return o;
}

static int test_add_and_cancel() {
    OrderBook book("AAPL");
    assert(book.order_count() == 0);

    Order buy = make_order(Side::BUY, OrderType::LIMIT, 150.0, 100);
    Order result = book.add_order(buy);

    assert(result.status == OrderStatus::NEW);
    assert(book.order_count() == 1);
    assert(book.best_bid() == 150.0);
    assert(book.bid_size() == 100);

    /* Cancel */
    assert(book.cancel_order(buy.id));
    assert(book.order_count() == 0);
    assert(book.best_bid() == 0.0);

    printf("  [PASS] test_add_and_cancel\n");
    return 0;
}

static int test_matching() {
    OrderBook book("AAPL");

    /* Add a sell order first */
    Order sell = make_order(Side::SELL, OrderType::LIMIT, 151.0, 50);
    book.add_order(sell);

    /* Add a buy order that crosses */
    Order buy = make_order(Side::BUY, OrderType::LIMIT, 152.0, 100);
    Order result = book.add_order(buy);

    /* 50 should be filled, 50 remaining */
    assert(result.filled_qty == 50);
    assert(result.status == OrderStatus::PARTIAL);

    /* The remaining 50 should be resting on bid side */
    assert(book.best_bid() == 152.0);
    assert(book.bid_size() == 50);

    /* A trade should have been generated */
    assert(book.trade_count() == 1);
    auto trades = book.drain_trades();
    assert(trades.size() == 1);
    assert(trades[0].price == 151.0);
    assert(trades[0].quantity == 50);

    printf("  [PASS] test_matching\n");
    return 0;
}

static int test_full_fill() {
    OrderBook book("AAPL");

    /* Add resting sell */
    Order sell = make_order(Side::SELL, OrderType::LIMIT, 100.0, 200);
    book.add_order(sell);

    /* Aggressive buy fills completely */
    Order buy = make_order(Side::BUY, OrderType::LIMIT, 100.0, 200);
    Order result = book.add_order(buy);

    assert(result.status == OrderStatus::FILLED);
    assert(result.filled_qty == 200);
    assert(book.order_count() == 0); /* All resting orders consumed */
    assert(book.trade_count() == 1);

    printf("  [PASS] test_full_fill\n");
    return 0;
}

static int test_price_time_priority() {
    OrderBook book("AAPL");

    /* Add two sell orders at the same price */
    Order sell1 = make_order(Side::SELL, OrderType::LIMIT, 100.0, 50);
    sell1.id = 2001;
    Order sell2 = make_order(Side::SELL, OrderType::LIMIT, 100.0, 50);
    sell2.id = 2002;

    book.add_order(sell1);
    book.add_order(sell2);

    /* Aggressive buy for 50 — should match sell1 (first in time) */
    Order buy = make_order(Side::BUY, OrderType::LIMIT, 100.0, 50);
    Order result = book.add_order(buy);

    assert(result.filled_qty == 50);

    /* sell1 should be filled, sell2 still resting */
    auto trades = book.drain_trades();
    assert(trades.size() == 1);
    assert(trades[0].sell_order_id == 2001); /* First in time */

    printf("  [PASS] test_price_time_priority\n");
    return 0;
}

static int test_market_order() {
    OrderBook book("AAPL");

    /* Add liquidity */
    Order sell1 = make_order(Side::SELL, OrderType::LIMIT, 100.0, 50);
    Order sell2 = make_order(Side::SELL, OrderType::LIMIT, 101.0, 50);
    book.add_order(sell1);
    book.add_order(sell2);

    /* Market buy — should fill against cheapest sells first */
    Order market_buy = make_order(Side::BUY, OrderType::MARKET, 0, 60);
    Order result = book.add_market_order(market_buy);

    assert(result.filled_qty == 60);
    assert(result.status == OrderStatus::FILLED);

    /* 50@100 + 10@101 = 60, leaving 40@101 */
    assert(book.best_ask() == 101.0);
    assert(book.ask_size() == 40);

    printf("  [PASS] test_market_order\n");
    return 0;
}

static int test_modify_order() {
    OrderBook book("AAPL");

    Order buy = make_order(Side::BUY, OrderType::LIMIT, 150.0, 100);
    book.add_order(buy);

    /* Modify price */
    Order modified = book.modify_order(buy.id, 151.0, 50);
    assert(modified.status == OrderStatus::NEW);
    assert(book.best_bid() == 151.0);
    assert(book.bid_size() == 50);

    printf("  [PASS] test_modify_order\n");
    return 0;
}

static int test_spread_and_mid() {
    OrderBook book("AAPL");

    book.add_order(make_order(Side::BUY, OrderType::LIMIT, 99.0, 100));
    book.add_order(make_order(Side::SELL, OrderType::LIMIT, 101.0, 100));

    assert(book.best_bid() == 99.0);
    assert(book.best_ask() == 101.0);
    assert(book.spread() == 2.0);
    assert(book.mid_price() == 100.0);

    printf("  [PASS] test_spread_and_mid\n");
    return 0;
}

int main() {
    printf("Order Book Tests:\n");
    test_add_and_cancel();
    test_matching();
    test_full_fill();
    test_price_time_priority();
    test_market_order();
    test_modify_order();
    test_spread_and_mid();
    printf("All order book tests passed.\n");
    return 0;
}
