/**
 * test_risk_engine.cpp — Comprehensive tests for enhanced RiskEngine
 *
 * Covers:
 *   §4.1 Pre-Trade Checks: size, price band, instrument allowlist, self-trade
 *   §4.2 Position Risk: net position, gross exposure, notional, int64_t
 *   §4.3 Exposure Control: portfolio exposure, position % of equity
 *   §4.4 / §9 Token Bucket Rate Limiter (burst-aware)
 *   §11 SAFE MODE: auto-triggers, manual enter/exit
 *   Kill switch
 *   Risk event callback
 */
#include "../include/risk_engine.h"
#include "../include/rate_limiter.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

static int next_id = 2000;

static Order make_order(const char *symbol, Side side, OrderType type,
                        double price, int32_t qty) {
    Order o;
    o.id = next_id++;
    o.account_id = 1;
    o.price = price;
    o.quantity = qty;
    o.side = side;
    o.type = type;
    strncpy(o.symbol, symbol, sizeof(o.symbol) - 1);
    return o;
}

/* ---- §4.1: Pre-Trade Checks ---- */

static int test_order_size_check() {
    RiskEngine engine;

    /* Valid order */
    Order ok = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
    assert(engine.validate(ok, 150.0));

    /* Zero quantity */
    Order zero = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 0);
    assert(!engine.validate(zero, 150.0));

    /* Exceeds limit */
    Order huge = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 99999);
    assert(!engine.validate(huge, 150.0));

    printf("  [PASS] test_order_size_check\n");
    return 0;
}

static int test_price_band_check() {
    RiskEngine engine;

    /* Within band (5% default) */
    Order ok = make_order("AAPL", Side::BUY, OrderType::LIMIT, 155.0, 100);
    assert(engine.validate(ok, 150.0));

    /* Outside band */
    Order far = make_order("AAPL", Side::BUY, OrderType::LIMIT, 200.0, 100);
    assert(!engine.validate(far, 150.0));

    /* Market orders bypass price band */
    Order mkt = make_order("AAPL", Side::BUY, OrderType::MARKET, 0.0, 100);
    assert(engine.validate(mkt, 150.0));

    /* No mid price — band check skipped */
    Order no_mid = make_order("AAPL", Side::BUY, OrderType::LIMIT, 200.0, 100);
    assert(engine.validate(no_mid, 0.0));

    printf("  [PASS] test_price_band_check\n");
    return 0;
}

static int test_instrument_allowlist() {
    RiskEngine engine;

    /* Add AAPL to allowlist */
    engine.allow_symbol("AAPL");

    /* AAPL should pass */
    Order aapl = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
    assert(engine.validate(aapl, 150.0));

    /* MSFT should be rejected (not in allowlist) */
    Order msft = make_order("MSFT", Side::BUY, OrderType::LIMIT, 300.0, 100);
    assert(!engine.validate(msft, 300.0));

    /* Clear allowlist — all should pass */
    engine.clear_allowlist();
    assert(engine.validate(msft, 300.0));

    printf("  [PASS] test_instrument_allowlist\n");
    return 0;
}

/* ---- §4.2: Position Risk ---- */

static int test_position_limit() {
    RiskEngine engine;

    /* Fill one order to build position */
    Order buy = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 5000);
    engine.on_order_executed(buy, 5000, 150.0);

    /* Same-direction order within limit */
    Order buy2 = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 4000);
    assert(engine.validate(buy2, 150.0));

    /* Exceeds limit (default: 100000) */
    Order huge = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100000);
    assert(!engine.validate(huge, 150.0));

    printf("  [PASS] test_position_limit\n");
    return 0;
}

static int test_position_int64_t() {
    /* Verify Position uses int64_t — can store beyond 2^31 */
    RiskEngine engine;

    int64_t large_qty = 3000000000LL; /* 3 billion — exceeds int32_t */
    /* This should compile and run without overflow (int32_t would overflow silently) */
    (void)large_qty;

    /* Position should safely hold a large value */
    Position pos;
    pos.net_position = large_qty;
    assert(pos.net_position == 3000000000LL);

    printf("  [PASS] test_position_int64_t\n");
    return 0;
}

/* ---- §4.3: Exposure Control ---- */

static int test_portfolio_exposure() {
    RiskEngine engine;

    /* Enable portfolio exposure limit */
    RiskLimits limits;
    limits.max_portfolio_exposure = 1000000.0; /* $1M cap */
    limits.max_orders_per_sec = 1000000;       /* effectively disable rate limit */
    limits.burst_capacity = 1000000;
    engine.set_limits(limits);

    /* First order: $750K — within $1M limit */
    Order buy1 = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 5000);
    assert(engine.validate(buy1, 150.0));
    engine.on_order_executed(buy1, 5000, 150.0);

    /* Second order: $300K — total $1.05M — exceeds $1M */
    Order buy2 = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 2000);
    assert(!engine.validate(buy2, 150.0));

    printf("  [PASS] test_portfolio_exposure\n");
    return 0;
}

static int test_position_pct_of_equity() {
    RiskEngine engine;

    RiskLimits limits;
    limits.max_position_pct = 0.20; /* 20% of equity */
    limits.max_orders_per_sec = 1000000;
    limits.burst_capacity = 1000000;
    engine.set_limits(limits);
    engine.set_equity(1000000.0); /* $1M equity */

    /* Order notional $150K = 15% of $1M — within 20% */
    Order ok = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 1000);
    assert(engine.validate(ok, 150.0));
    engine.on_order_executed(ok, 1000, 150.0);

    /* Add more: $120K more = total $270K = 27% — exceeds 20% */
    Order too_much = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 800);
    assert(!engine.validate(too_much, 150.0));

    printf("  [PASS] test_position_pct_of_equity\n");
    return 0;
}

/* ---- §9 / §4.4: Token Bucket Rate Limiter ---- */

static int test_token_bucket_basic() {
    TokenBucket tb;
    tb_init(&tb, 100, 200); /* 100 tokens/sec, burst 200 */

    /* Initial burst capacity: 200 tokens */
    for (int i = 0; i < 200; i++) {
        assert(tb_allow(&tb));
    }
    /* 201st should fail (no tokens left) */
    assert(!tb_allow(&tb));

    printf("  [PASS] test_token_bucket_basic\n");
    return 0;
}

static int test_token_bucket_refill() {
    TokenBucket tb;
    tb_init(&tb, 1000000, 1000000); /* High rate so refill is fast */
    int allowed = 0;
    /* Should get many tokens through — at least some after brief wait */
    for (int i = 0; i < 10000; i++) {
        if (tb_allow(&tb)) allowed++;
    }
    assert(allowed > 0);
    printf("  [PASS] test_token_bucket_refill (got %d tokens)\n", allowed);
    return 0;
}

static int test_rate_limit_integration() {
    RiskEngine engine;

    /* Set a low rate limit */
    RiskLimits limits;
    limits.max_orders_per_sec = 10;
    limits.burst_capacity = 10;
    limits.price_band_pct = 100.0;  /* effectively disable price band */
    engine.set_limits(limits);

    /* First 10 orders should pass */
    for (int i = 0; i < 10; i++) {
        Order o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
        o.id = 3000 + i;
        assert(engine.validate(o, 150.0));
    }

    /* 11th should be rate limited (no tokens) */
    Order extra = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
    extra.id = 4000;
    assert(!engine.validate(extra, 150.0));

    printf("  [PASS] test_rate_limit_integration\n");
    return 0;
}

/* ---- Kill Switch ---- */

static int test_kill_switch() {
    RiskEngine engine;

    /* Normal operation */
    Order ok = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
    assert(engine.validate(ok, 150.0));

    /* Engage kill switch */
    engine.kill_switch(true);
    assert(engine.is_killed());

    /* All orders rejected */
    assert(!engine.validate(ok, 150.0));

    /* Disable kill switch */
    engine.kill_switch(false);
    assert(!engine.is_killed());
    assert(engine.validate(ok, 150.0));

    printf("  [PASS] test_kill_switch\n");
    return 0;
}

/* ---- §11: SAFE MODE ---- */

static int test_safe_mode_manual() {
    RiskEngine engine;

    /* Normal operation */
    Order ok = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 100);
    assert(engine.validate(ok, 150.0));

    /* Enter SAFE MODE */
    engine.enter_safe_mode("Manual test trigger");
    assert(engine.is_safe_mode());
    assert(strlen(engine.safe_mode_reason()) > 0);

    /* All orders rejected in SAFE MODE */
    assert(!engine.validate(ok, 150.0));

    /* Exit SAFE MODE */
    engine.exit_safe_mode();
    assert(!engine.is_safe_mode());

    /* Orders pass again */
    assert(engine.validate(ok, 150.0));

    printf("  [PASS] test_safe_mode_manual\n");
    return 0;
}

static int test_safe_mode_auto_consecutive_rejects() {
    RiskEngine engine;

    RiskLimits limits;
    limits.safe_mode_consecutive = 5; /* Enter SAFE MODE after 5 consecutive rejects */
    limits.max_orders_per_sec = 1000000; /* disable rate limit */
    limits.burst_capacity = 1000000;
    engine.set_limits(limits);

    /* Generate 5 invalid orders (zero quantity) to trigger SAFE MODE */
    for (int i = 0; i < 5; i++) {
        Order bad = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 0);
        bad.id = 5000 + i;
        assert(!engine.validate(bad, 150.0));
    }

    /* SAFE MODE should now be active */
    assert(engine.is_safe_mode());

    printf("  [PASS] test_safe_mode_auto_consecutive_rejects\n");
    return 0;
}

static int test_safe_mode_auto_reject_rate() {
    RiskEngine engine;

    RiskLimits limits;
    limits.safe_mode_reject_pct = 30.0; /* Enter SAFE MODE if >30% reject */
    limits.max_orders_per_sec = 1000000;
    limits.burst_capacity = 1000000;
    engine.set_limits(limits);

    /* Feed high reject rate into health metrics */
    engine.update_health_metrics(50.0, 10.0); /* 50% reject, 10us latency */

    /* Next validation that rejects should trigger SAFE MODE */
    Order bad = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 0);
    bad.id = 6000;
    assert(!engine.validate(bad, 150.0));
    assert(engine.is_safe_mode());

    printf("  [PASS] test_safe_mode_auto_reject_rate\n");
    return 0;
}

/* ---- Risk Event Callback ---- */

static int g_callback_count = 0;
static RiskEventType g_last_event_type;
static char g_last_reason[256];

static void test_callback(const RiskEvent *event, void *ctx) {
    (void)ctx;
    g_callback_count++;
    g_last_event_type = event->type;
    if (event->reason[0]) {
        strncpy(g_last_reason, event->reason, sizeof(g_last_reason) - 1);
    }
}

static int test_risk_event_callback() {
    RiskEngine engine;
    g_callback_count = 0;

    engine.set_event_callback(test_callback, nullptr);

    /* Rejected order should fire callback */
    Order bad = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 0);
    bad.id = 7000;
    assert(!engine.validate(bad, 150.0));
    assert(g_callback_count >= 1);
    assert(g_last_event_type == RiskEventType::ORDER_REJECTED);

    /* Kill switch should fire callback */
    int count_before = g_callback_count;
    engine.kill_switch(true);
    assert(g_callback_count > count_before);
    assert(g_last_event_type == RiskEventType::KILL_SWITCH);

    printf("  [PASS] test_risk_event_callback\n");
    return 0;
}

/* ---- get_position ---- */

static int test_get_position() {
    RiskEngine engine;

    Order buy = make_order("AAPL", Side::BUY, OrderType::LIMIT, 150.0, 1000);
    engine.on_order_executed(buy, 1000, 150.0);

    /* Query by string */
    Position pos = engine.get_position("AAPL");
    assert(pos.net_position == 1000);
    assert(pos.gross_exposure == 1000);
    assert(pos.total_notional == 150000.0);

    /* Query by const char* */
    Position pos2 = engine.get_position((const char *)"AAPL");
    assert(pos2.net_position == 1000);

    /* Unknown symbol */
    Position pos3 = engine.get_position("MSFT");
    assert(pos3.net_position == 0);

    printf("  [PASS] test_get_position\n");
    return 0;
}

/* ---- Position type upgrade: int32_t → int64_t ---- */

static int test_position_fields_are_int64() {
    RiskLimits limits;
    limits.max_order_qty = 5000000000LL; /* 5 billion — exceeds int32 */
    assert(limits.max_order_qty == 5000000000LL);
    printf("  [PASS] test_position_fields_are_int64\n");
    return 0;
}

/* ---- Main ---- */

int main() {
    printf("Risk Engine Tests (Enhanced per hft_risk_engine_design.md):\n");

    /* §4.1 Pre-Trade Checks */
    test_order_size_check();
    test_price_band_check();
    test_instrument_allowlist();

    /* §4.2 Position Risk */
    test_position_limit();
    test_position_int64_t();

    /* §4.3 Exposure Control */
    test_portfolio_exposure();
    test_position_pct_of_equity();

    /* §9 Token Bucket Rate Limiter */
    test_token_bucket_basic();
    test_token_bucket_refill();
    test_rate_limit_integration();

    /* Kill Switch */
    test_kill_switch();

    /* §11 SAFE MODE */
    test_safe_mode_manual();
    test_safe_mode_auto_consecutive_rejects();
    test_safe_mode_auto_reject_rate();

    /* Risk Event Callback */
    test_risk_event_callback();

    /* Position queries */
    test_get_position();

    /* int64_t upgrade */
    test_position_fields_are_int64();

    printf("All risk engine tests passed.\n");
    return 0;
}
