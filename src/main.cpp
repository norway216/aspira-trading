/**
 * main.cpp — High-Performance Trading System Entry Point
 *
 * Aspira Trading Engine
 * A production-grade low-latency trading system.
 *
 * Usage:
 *   ./trading_engine [--duration <seconds>] [--symbol <sym>] [--log <path>]
 *
 * The system runs with:
 *   - Simulated market data feed (2000 msgs/sec)
 *   - Strategy engine generating buy/sell orders
 *   - Price-time priority order book with matching
 *   - Risk engine validating all orders
 *   - Execution gateway dispatching orders
 *   - Async logger and persistence journal
 *
 * Press Ctrl+C to stop gracefully.
 */
#include "event_pipeline.h"
#include "journal.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <unistd.h>

/* Global for signal handling */
static EventPipeline *g_pipeline = nullptr;
static journal_t *g_journal = nullptr;

/* ---- Signal Handler ---- */

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n\n*** Signal received — shutting down gracefully ***\n\n");

    if (g_pipeline) {
        g_pipeline->stop();
    }
    if (g_journal) {
        journal_close(g_journal);
        g_journal = nullptr;
    }
}

/* ---- Print Banner ---- */

static void print_banner() {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Aspira High-Performance Trading Engine        ║\n");
    printf("║                    Version 1.0.0                       ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Architecture:                                        ║\n");
    printf("║    Market Data → Strategy → Risk → Book → Execution   ║\n");
    printf("║                                                       ║\n");
    printf("║  Key Features:                                        ║\n");
    printf("║    • Lock-free SPSC ring buffers                      ║\n");
    printf("║    • Price-time priority order book                   ║\n");
    printf("║    • Pre-trade risk validation                        ║\n");
    printf("║    • Async logging & persistence journal              ║\n");
    printf("║    • Event-driven pipeline architecture               ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ---- Print Running Stats ---- */

static void print_stats(const EventPipeline &pipeline) {
    PipelineStats s = pipeline.stats();

    printf("\r");
    printf("  Market Data: %8lu | Orders: %6lu | Accepted: %6lu | "
           "Rejected: %4lu | Trades: %6lu | Latency: %7.2f µs",
           (unsigned long)s.market_data_msgs,
           (unsigned long)s.orders_created,
           (unsigned long)s.orders_accepted,
           (unsigned long)s.orders_rejected,
           (unsigned long)s.trades_generated,
           s.avg_latency_us);
    fflush(stdout);
}

/* ---- Usage ---- */

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -d, --duration <sec>   Run duration in seconds (default: 10, 0 = infinite)\n");
    printf("  -s, --symbol <sym>     Trading symbol (default: AAPL)\n");
    printf("  -l, --log <path>       Log file path (default: trading_engine.log)\n");
    printf("  -j, --journal <path>   Journal file path (default: trading_engine.jrnl)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\n");
    printf("Press Ctrl+C to stop gracefully.\n");
    printf("\n");
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    /* Default settings */
    int duration_sec = 10;
    const char *symbol = "AAPL";
    const char *log_path = "trading_engine.log";
    const char *journal_path = "trading_engine.jrnl";

    /* Parse command line */
    static struct option long_options[] = {
        {"duration", required_argument, 0, 'd'},
        {"symbol",   required_argument, 0, 's'},
        {"log",      required_argument, 0, 'l'},
        {"journal",  required_argument, 0, 'j'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:s:l:j:h", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'd': duration_sec = atoi(optarg); break;
        case 's': symbol = optarg; break;
        case 'l': log_path = optarg; break;
        case 'j': journal_path = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    print_banner();

    printf("Configuration:\n");
    printf("  Symbol:     %s\n", symbol);
    printf("  Duration:   %d seconds%s\n", duration_sec,
           duration_sec == 0 ? " (infinite)" : "");
    printf("  Log:        %s\n", log_path);
    printf("  Journal:    %s\n", journal_path);
    printf("\n");

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open persistence journal */
    g_journal = journal_open(journal_path, true);
    if (!g_journal) {
        fprintf(stderr, "Warning: failed to open journal at %s\n", journal_path);
    } else {
        printf("Journal opened: %s (mmap mode)\n", journal_path);
    }

    /* Initialize pipeline */
    EventPipeline pipeline;
    g_pipeline = &pipeline;

    printf("Initializing pipeline...\n");
    if (pipeline.init(symbol, log_path) != 0) {
        fprintf(stderr, "FATAL: Pipeline initialization failed\n");
        if (g_journal) {
            journal_close(g_journal);
            g_journal = nullptr;
        }
        return 1;
    }

    /* Attach journal to pipeline for persistence */
    if (g_journal) {
        pipeline.set_journal(g_journal);
    }

    /* Start pipeline */
    printf("Starting trading engine...\n\n");
    if (pipeline.start() != 0) {
        fprintf(stderr, "FATAL: Pipeline start failed\n");
        if (g_journal) {
            journal_close(g_journal);
            g_journal = nullptr;
        }
        return 1;
    }

    printf("Trading engine is running. ");
    if (duration_sec > 0) {
        printf("Will run for %d seconds...\n\n", duration_sec);
    } else {
        printf("Running indefinitely (Ctrl+C to stop)...\n\n");
    }

    /* Main loop — print stats periodically */
    time_t start_time = time(nullptr);
    while (true) {
        sleep(1);

        /* Print live stats */
        print_stats(pipeline);

        /* Check duration */
        if (duration_sec > 0 && time(nullptr) - start_time >= duration_sec) {
            printf("\n\nDuration reached — shutting down...\n");
            break;
        }
    }

    printf("\n");

    /* Stop pipeline */
    pipeline.stop();

    /* Final stats */
    PipelineStats final_stats = pipeline.stats();
    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║              Final Performance Report                 ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Market Data Messages:  %10lu                  ║\n",
           (unsigned long)final_stats.market_data_msgs);
    printf("║  Orders Created:        %10lu                  ║\n",
           (unsigned long)final_stats.orders_created);
    printf("║  Orders Accepted:       %10lu                  ║\n",
           (unsigned long)final_stats.orders_accepted);
    printf("║  Orders Rejected:       %10lu                  ║\n",
           (unsigned long)final_stats.orders_rejected);
    printf("║  Orders Executed:       %10lu                  ║\n",
           (unsigned long)final_stats.orders_executed);
    printf("║  Trades Generated:      %10lu                  ║\n",
           (unsigned long)final_stats.trades_generated);
    printf("║  Average Latency:       %8.2f µs               ║\n",
           final_stats.avg_latency_us);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Throughput:            %8lu msgs/sec          ║\n",
           duration_sec > 0
               ? (unsigned long)(final_stats.market_data_msgs / (uint64_t)duration_sec)
               : 0UL);
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");

    /* Close journal */
    if (g_journal) {
        uint64_t jrnl_bytes = journal_bytes_written(g_journal);
        uint64_t jrnl_seq = journal_sequence(g_journal);
        journal_close(g_journal);
        g_journal = nullptr;
        printf("Journal closed: %lu bytes, %lu records\n",
               (unsigned long)jrnl_bytes, (unsigned long)jrnl_seq);
    }

    printf("\nTrading engine shut down successfully.\n\n");
    return 0;
}
