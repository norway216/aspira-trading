#!/usr/bin/env bash
#
# bench/benchmark.sh — Aspira Trading Engine Performance Benchmark
#
# Runs the trading engine under various conditions and measures:
#   - Latency (average, P50, P99)
#   - Throughput (messages/sec)
#   - Order processing rate
#   - Trade generation rate
#   - CPU and memory usage
#   - Journal write throughput
#
# Usage:
#   ./bench/benchmark.sh [OPTIONS]
#
# Options:
#   -d, --duration <SEC>   Duration per benchmark run (default: 10)
#   -r, --runs <N>         Number of runs per scenario (default: 3)
#   -w, --warmup <SEC>     Warmup duration before measurement (default: 2)
#   -s, --symbols <LIST>   Comma-separated symbols to test (default: AAPL)
#   -o, --output <FILE>    Output report file (default: bench_report.txt)
#   -q, --quick            Quick mode — shorter runs, fewer iterations
#   -h, --help             Show this help

set -euo pipefail

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

# ---- Defaults ----
DURATION=10
NUM_RUNS=3
WARMUP=2
SYMBOLS="AAPL"
OUTPUT_FILE=""
QUICK_MODE=false

# ---- Paths ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
ENGINE_BIN="${BUILD_DIR}/trading_engine"
BENCH_DIR="${SCRIPT_DIR}"
LOG_DIR="${BENCH_DIR}/logs"
REPORT_FILE=""

# ---- Helpers ----
log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${BOLD}${CYAN}▶ $*${NC}"; }

# ---- Usage ----
usage() {
    grep '^#' "$0" | grep -E '^(#  |#   )' | sed 's/^# //' | sed 's/^#$//'
    exit 0
}

# ---- Parse arguments ----
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d|--duration) DURATION="$2"; shift 2 ;;
            -r|--runs)     NUM_RUNS="$2"; shift 2 ;;
            -w|--warmup)   WARMUP="$2";   shift 2 ;;
            -s|--symbols)  SYMBOLS="$2";  shift 2 ;;
            -o|--output)   OUTPUT_FILE="$2"; shift 2 ;;
            -q|--quick)    QUICK_MODE=true; shift ;;
            -h|--help)     usage ;;
            *) log_error "Unknown option: $1"; usage ;;
        esac
    done

    if [[ "$QUICK_MODE" == true ]]; then
        DURATION=3
        NUM_RUNS=1
        WARMUP=1
    fi

    REPORT_FILE="${OUTPUT_FILE:-${BENCH_DIR}/bench_report_$(date +%Y%m%d_%H%M%S).txt}"
}

# ---- Check engine binary ----
check_binary() {
    if [[ ! -x "$ENGINE_BIN" ]]; then
        log_error "Engine binary not found or not executable: $ENGINE_BIN"
        log_info "Build first: cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)"
        exit 1
    fi

    local bin_size
    bin_size=$(du -h "$ENGINE_BIN" | cut -f1)
    log_info "Engine binary: $ENGINE_BIN (${bin_size})"
}

# ---- Setup benchmark environment ----
setup() {
    mkdir -p "$LOG_DIR"

    # Clean up any leftover logs from previous runs
    rm -f "${BUILD_DIR}"/trading_engine.log
    rm -f "${BUILD_DIR}"/trading_engine.jrnl
    rm -f "${BUILD_DIR}"/execution_gateway.log

    log_ok "Benchmark environment ready"
}

# ---- Parse engine output ----
parse_output() {
    local logfile="$1"
    local -n out_ref=$2  # nameref to associative array

    # Extract final stats from the engine's stdout
    out_ref[market_data]=$(grep -oP 'Market Data Messages:\s+\K\d+' "$logfile" || echo "0")
    out_ref[orders_created]=$(grep -oP 'Orders Created:\s+\K\d+' "$logfile" || echo "0")
    out_ref[orders_accepted]=$(grep -oP 'Orders Accepted:\s+\K\d+' "$logfile" || echo "0")
    out_ref[orders_rejected]=$(grep -oP 'Orders Rejected:\s+\K\d+' "$logfile" || echo "0")
    out_ref[orders_executed]=$(grep -oP 'Orders Executed:\s+\K\d+' "$logfile" || echo "0")
    out_ref[trades]=$(grep -oP 'Trades Generated:\s+\K\d+' "$logfile" || echo "0")
    out_ref[latency]=$(grep -oP 'Average Latency:\s+\K[\d.]+' "$logfile" || echo "0")
    out_ref[throughput]=$(grep -oP 'Throughput:\s+\K\d+' "$logfile" || echo "0")
    out_ref[journal_bytes]=$(grep -oP 'Journal closed:\s+\K\d+' "$logfile" || echo "0")
    out_ref[journal_records]=$(grep -oP 'Journal closed: \d+ bytes, \K\d+' "$logfile" || echo "0")

    # Calculate acceptance rate
    local created=${out_ref[orders_created]}
    local accepted=${out_ref[orders_accepted]}
    if [[ "$created" -gt 0 ]]; then
        out_ref[accept_rate]=$(awk "BEGIN {printf \"%.1f\", ($accepted/$created)*100}")
    else
        out_ref[accept_rate]="0"
    fi

    # Calculate trade rate (trades per second)
    local trades=${out_ref[trades]}
    local duration=${out_ref[duration]:-$DURATION}
    if [[ "$duration" -gt 0 ]]; then
        out_ref[trade_rate]=$(awk "BEGIN {printf \"%.1f\", $trades/$duration}")
    else
        out_ref[trade_rate]="0"
    fi
}

# ---- Run a single benchmark ----
# Returns: result file path on stdout; status messages on stderr
run_benchmark() {
    local symbol="$1"
    local duration="$2"
    local run_id="$3"
    local logfile="${LOG_DIR}/bench_${symbol}_run${run_id}_$(date +%H%M%S).log"

    printf "    Run %s (%ss) ... " "$run_id" "$duration" >&2

    # Run engine and capture output
    if timeout $((duration + 120)) "$ENGINE_BIN" \
        --duration "$duration" \
        --symbol "$symbol" \
        --log "${BUILD_DIR}/trading_engine.log" \
        --journal "${BUILD_DIR}/trading_engine.jrnl" \
        > "$logfile" 2>&1; then
        echo -e "${GREEN}OK${NC}" >&2
    else
        local exit_code=$?
        if [[ $exit_code -eq 124 ]]; then
            echo -e "${RED}TIMEOUT${NC}" >&2
        else
            echo -e "${RED}FAILED (exit=$exit_code)${NC}" >&2
        fi
        return 1
    fi

    # Parse results
    local -A results
    results[duration]="$duration"
    parse_output "$logfile" results

    # Print per-run summary to stderr
    printf "      Latency: %8.2f µs  |  Throughput: %6s msg/s  |  Trades: %6s\n" \
        "${results[latency]}" "${results[throughput]}" "${results[trades]}" >&2

    # Write result file
    local result_file="${LOG_DIR}/.result_${symbol}_${run_id}"
    {
        echo "results[duration]=${duration}"
        echo "results[market_data]=${results[market_data]}"
        echo "results[orders_created]=${results[orders_created]}"
        echo "results[orders_accepted]=${results[orders_accepted]}"
        echo "results[orders_rejected]=${results[orders_rejected]}"
        echo "results[orders_executed]=${results[orders_executed]}"
        echo "results[trades]=${results[trades]}"
        echo "results[latency]=${results[latency]}"
        echo "results[throughput]=${results[throughput]}"
        echo "results[journal_bytes]=${results[journal_bytes]}"
        echo "results[journal_records]=${results[journal_records]}"
        echo "results[accept_rate]=${results[accept_rate]}"
        echo "results[trade_rate]=${results[trade_rate]}"
    } > "$result_file"

    # Only output the result file path on stdout
    echo "$result_file"
}

# ---- Measure CPU and memory during a run ----
measure_resources() {
    local symbol="$1"
    local duration="$2"
    local pid

    # Start engine in background
    "$ENGINE_BIN" --duration "$duration" --symbol "$symbol" \
        > /dev/null 2>&1 &
    pid=$!

    sleep 1  # Let it start

    local cpu_samples=()
    local mem_samples=()
    local start_time
    start_time=$(date +%s)
    local end_time=$((start_time + duration))

    while [[ $(date +%s) -lt $end_time ]] && kill -0 "$pid" 2>/dev/null; do
        local cpu_mem
        cpu_mem=$(ps -p "$pid" -o %cpu=,%mem= 2>/dev/null || echo "0 0")
        local cpu
        cpu=$(echo "$cpu_mem" | awk '{print $1}')
        local mem
        mem=$(echo "$cpu_mem" | awk '{print $2}')
        cpu_samples+=("${cpu:-0}")
        mem_samples+=("${mem:-0}")
        sleep 0.5
    done

    wait "$pid" 2>/dev/null || true

    # Calculate averages
    local cpu_avg=0 mem_avg=0 cpu_max=0 mem_max=0
    if [[ ${#cpu_samples[@]} -gt 0 ]]; then
        cpu_avg=$(awk "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.1f\", s/${#cpu_samples[@]}}" "${cpu_samples[@]}")
        mem_avg=$(awk "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.1f\", s/${#mem_samples[@]}}" "${mem_samples[@]}")

        cpu_max=$(printf '%s\n' "${cpu_samples[@]}" | sort -rn | head -1)
        mem_max=$(printf '%s\n' "${mem_samples[@]}" | sort -rn | head -1)
    fi

    echo "${cpu_avg}|${mem_avg}|${cpu_max}|${mem_max}"
}

# ---- Write report header ----
write_report_header() {
    {
        echo "═══════════════════════════════════════════════════════════════"
        echo "  Aspira Trading Engine — Performance Benchmark Report"
        echo "═══════════════════════════════════════════════════════════════"
        echo ""
        echo "  Date:       $(date '+%Y-%m-%d %H:%M:%S')"
        echo "  Host:       $(hostname)"
        echo "  CPU:        $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo 'unknown')"
        echo "  Cores:      $(nproc)"
        echo "  Memory:     $(free -h 2>/dev/null | awk '/Mem:/{print $2}' || echo 'unknown')"
        echo "  OS:         $(uname -sr)"
        echo "  Kernel:     $(uname -r)"
        echo ""
        echo "  Binary:     $ENGINE_BIN"
        echo "  Build:      $(file "$ENGINE_BIN" 2>/dev/null | cut -d: -f2- | xargs)"
        echo ""
        echo "  Parameters:"
        echo "    Duration: $DURATION s"
        echo "    Runs:     $NUM_RUNS"
        echo "    Warmup:   $WARMUP s"
        echo "    Symbols:  $SYMBOLS"
        echo ""
        echo "───────────────────────────────────────────────────────────────"
        echo ""
    } > "$REPORT_FILE"
}

# ---- Run benchmark for one symbol ----
benchmark_symbol() {
    local symbol="$1"
    local -gA agg_results
    local result_files=()

    log_step "Benchmarking symbol: ${BOLD}${symbol}${NC}"

    # Warmup
    if [[ "$WARMUP" -gt 0 ]]; then
        echo -n "  Warming up (${WARMUP}s) ... " >&2
        if "$ENGINE_BIN" --duration "$WARMUP" --symbol "$symbol" > /dev/null 2>&1; then
            echo -e "${GREEN}OK${NC}" >&2
        else
            echo -e "${YELLOW}SKIPPED${NC}" >&2
        fi
        sleep 1
    fi

    # Measurement runs
    for ((run=1; run<=NUM_RUNS; run++)); do
        local rf
        rf=$(run_benchmark "$symbol" "$DURATION" "$run")
        if [[ -n "$rf" ]]; then
            result_files+=("$rf")
        fi
        sleep 1
    done

    # Aggregate results
    if [[ ${#result_files[@]} -eq 0 ]]; then
        log_error "No successful runs for $symbol"
        return 1
    fi

    local -a latencies=() throughputs=() trade_rates=() accept_rates=()
    local total_journal_bytes=0 total_journal_records=0

    for rf in "${result_files[@]}"; do
        # Read values from result file (key=value format, one per line)
        local _latency _throughput _trade_rate _accept_rate _jrnl_bytes _jrnl_records
        _latency=$(grep '^results\[latency\]=' "$rf" | cut -d= -f2)
        _throughput=$(grep '^results\[throughput\]=' "$rf" | cut -d= -f2)
        _trade_rate=$(grep '^results\[trade_rate\]=' "$rf" | cut -d= -f2)
        _accept_rate=$(grep '^results\[accept_rate\]=' "$rf" | cut -d= -f2)
        _jrnl_bytes=$(grep '^results\[journal_bytes\]=' "$rf" | cut -d= -f2)
        _jrnl_records=$(grep '^results\[journal_records\]=' "$rf" | cut -d= -f2)

        latencies+=("${_latency:-0}")
        throughputs+=("${_throughput:-0}")
        trade_rates+=("${_trade_rate:-0}")
        accept_rates+=("${_accept_rate:-0}")
        total_journal_bytes=$((total_journal_bytes + ${_jrnl_bytes:-0}))
        total_journal_records=$((total_journal_records + ${_jrnl_records:-0}))
    done

    # Calculate statistics
    local avg_lat min_lat max_lat avg_tp avg_tr avg_ar
    avg_lat=$(awk "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.2f\", s/${#latencies[@]}}" "${latencies[@]}")
    avg_tp=$(awk  "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.0f\", s/${#throughputs[@]}}" "${throughputs[@]}")
    avg_tr=$(awk  "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.0f\", s/${#trade_rates[@]}}" "${trade_rates[@]}")
    avg_ar=$(awk  "BEGIN {s=0; for(i in ARGV) s+=ARGV[i]; printf \"%.1f\", s/${#accept_rates[@]}}" "${accept_rates[@]}")

    # Sort for min/max
    min_lat=$(printf '%s\n' "${latencies[@]}" | sort -n | head -1)
    max_lat=$(printf '%s\n' "${latencies[@]}" | sort -n | tail -1)

    # Write to report
    {
        echo "───────────────────────────────────────────────────────────────"
        echo "  Symbol: $symbol"
        echo ""
        echo "  Latency:"
        echo "    Average:    ${avg_lat} µs"
        echo "    Min:        ${min_lat} µs"
        echo "    Max:        ${max_lat} µs"
        echo ""
        echo "  Throughput:"
        echo "    Messages:   ${avg_tp} msg/s"
        echo "    Trades:     ${avg_tr} trades/s"
        echo ""
        echo "  Orders:"
        echo "    Acceptance: ${avg_ar}%"
        echo ""
        echo "  Journal:"
        echo "    Bytes:      $(numfmt --to=iec ${total_journal_bytes} 2>/dev/null || echo ${total_journal_bytes})"
        echo "    Records:    ${total_journal_records}"
        echo ""
    } >> "$REPORT_FILE"

    # Print summary to console
    echo ""
    echo -e "  ${GREEN}${BOLD}Summary for ${symbol}:${NC}"
    echo -e "    Latency (avg/min/max):  ${CYAN}${avg_lat}${NC} / ${min_lat} / ${max_lat} µs"
    echo -e "    Throughput (msg/trade): ${CYAN}${avg_tp}${NC} / ${avg_tr} msg|trade/s"
    echo -e "    Acceptance rate:        ${CYAN}${avg_ar}%${NC}"
}

# ---- Resource monitoring section ----
benchmark_resources() {
    log_step "Resource usage measurement..."

    local first_symbol
    first_symbol=$(echo "$SYMBOLS" | cut -d, -f1)

    echo -n "  Measuring CPU and memory for ${first_symbol} (${DURATION}s) ... "
    local res
    res=$(measure_resources "$first_symbol" "$DURATION")
    echo -e "${GREEN}OK${NC}"

    local cpu_avg mem_avg cpu_max mem_max
    IFS='|' read -r cpu_avg mem_avg cpu_max mem_max <<< "$res"

    {
        echo "───────────────────────────────────────────────────────────────"
        echo "  Resource Usage (Symbol: ${first_symbol})"
        echo ""
        echo "  CPU:"
        echo "    Average:    ${cpu_avg}%"
        echo "    Peak:       ${cpu_max}%"
        echo ""
        echo "  Memory:"
        echo "    Average:    ${mem_avg}%"
        echo "    Peak:       ${mem_max}%"
        echo ""
    } >> "$REPORT_FILE"

    echo -e "    CPU  (avg/max):  ${CYAN}${cpu_avg}%${NC} / ${cpu_max}%"
    echo -e "    Mem  (avg/max):  ${CYAN}${mem_avg}%${NC} / ${mem_max}%"
}

# ---- Latency histogram from log data ----
latency_histogram() {
    log_step "Latency distribution analysis..."

    local logfile="${BUILD_DIR}/trading_engine.log"
    if [[ ! -f "$logfile" ]]; then
        log_warn "No engine log found, skipping latency histogram"
        return
    fi

    # Extract per-event latencies from the log
    # The log doesn't have per-event latencies directly; we run a quick analysis
    # using the engine's own latency tracking
    local analysis_log="${LOG_DIR}/latency_analysis.log"

    "$ENGINE_BIN" --duration 5 --symbol "$(echo "$SYMBOLS" | cut -d, -f1)" \
        > "$analysis_log" 2>&1 || true

    # Parse the live stats lines to get latency samples
    local -a lat_samples=()
    while IFS= read -r line; do
        if [[ "$line" =~ Latency:\ +([0-9.]+)\ µs ]]; then
            lat_samples+=("${BASH_REMATCH[1]}")
        fi
    done < "$analysis_log"

    if [[ ${#lat_samples[@]} -lt 3 ]]; then
        log_warn "Not enough latency samples for histogram"
        return
    fi

    # Sort and compute percentiles
    local sorted
    sorted=$(printf '%s\n' "${lat_samples[@]}" | sort -n)
    local count=${#lat_samples[@]}

    local p50_idx=$((count * 50 / 100))
    local p95_idx=$((count * 95 / 100))
    local p99_idx=$((count * 99 / 100))
    [[ $p50_idx -ge $count ]] && p50_idx=$((count - 1))
    [[ $p95_idx -ge $count ]] && p95_idx=$((count - 1))
    [[ $p99_idx -ge $count ]] && p99_idx=$((count - 1))

    local p50 p95 p99
    p50=$(echo "$sorted" | sed -n "${p50_idx}p")
    p95=$(echo "$sorted" | sed -n "${p95_idx}p")
    p99=$(echo "$sorted" | sed -n "${p99_idx}p")

    {
        echo "───────────────────────────────────────────────────────────────"
        echo "  Latency Distribution (${count} samples)"
        echo ""
        echo "  P50:  ${p50} µs"
        echo "  P95:  ${p95} µs"
        echo "  P99:  ${p99} µs"
        echo ""
    } >> "$REPORT_FILE"

    echo -e "    P50: ${CYAN}${p50}${NC} µs  |  P95: ${CYAN}${p95}${NC} µs  |  P99: ${CYAN}${p99}${NC} µs"

    # Print simple ASCII histogram
    echo ""
    echo -e "  ${BOLD}Latency Histogram:${NC}"
    local bins=(0 5 10 20 50 100 200 500)
    for ((i=0; i<${#bins[@]}-1; i++)); do
        local lo=${bins[$i]}
        local hi=${bins[$((i+1))]}
        local cnt=0
        for s in "${lat_samples[@]}"; do
            if awk "BEGIN {exit !($s >= $lo && $s < $hi)}"; then
                cnt=$((cnt + 1))
            fi
        done
        local bar
        bar=$(printf '%*s' $((cnt * 50 / count)) '' | tr ' ' '█')
        printf "    [%3d - %3d) µs: %s (%d)\n" "$lo" "$hi" "$bar" "$cnt"
    done
    local hi_cnt=0
    for s in "${lat_samples[@]}"; do
        if awk "BEGIN {exit !($s >= ${bins[-1]})}"; then
            hi_cnt=$((hi_cnt + 1))
        fi
    done
    local bar
    bar=$(printf '%*s' $((hi_cnt * 50 / count)) '' | tr ' ' '█')
    printf "    [%3d+     ) µs: %s (%d)\n" "${bins[-1]}" "$bar" "$hi_cnt"
    echo ""

    # Also write to report
    {
        echo "  Histogram:"
        for ((i=0; i<${#bins[@]}-1; i++)); do
            local lo=${bins[$i]}
            local hi=${bins[$((i+1))]}
            local cnt=0
            for s in "${lat_samples[@]}"; do
                if awk "BEGIN {exit !($s >= $lo && $s < $hi)}"; then
                    cnt=$((cnt + 1))
                fi
            done
            echo "    [${lo} - ${hi}) µs: ${cnt}"
        done
        echo ""
    } >> "$REPORT_FILE"
}

# ---- Main ----
main() {
    echo -e "${MAGENTA}${BOLD}"
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║   Aspira Trading Engine — Performance Benchmark      ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    echo ""

    parse_args "$@"
    check_binary
    setup
    write_report_header

    # Benchmark each symbol
    IFS=',' read -ra SYM_ARRAY <<< "$SYMBOLS"
    for sym in "${SYM_ARRAY[@]}"; do
        benchmark_symbol "$sym"
    done

    # Resource measurement
    benchmark_resources

    # Latency distribution
    latency_histogram

    # Final report
    {
        echo "═══════════════════════════════════════════════════════════════"
        echo "  Benchmark Complete — $(date '+%Y-%m-%d %H:%M:%S')"
        echo "═══════════════════════════════════════════════════════════════"
    } >> "$REPORT_FILE"

    echo ""
    echo -e "${GREEN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}${BOLD}║           Benchmark Complete                         ║${NC}"
    echo -e "${GREEN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  Report: ${CYAN}${REPORT_FILE}${NC}"
    echo -e "  Logs:   ${CYAN}${LOG_DIR}${NC}"
    echo ""
    echo -e "  View report: ${BOLD}cat ${REPORT_FILE}${NC}"
    echo ""
}

main "$@"
