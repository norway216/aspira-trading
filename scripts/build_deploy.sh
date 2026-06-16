#!/usr/bin/env bash
#
# scripts/build_deploy.sh — Aspira Trading Engine Build & Deploy Script
#
# Usage:
#   ./scripts/build_deploy.sh [OPTIONS]
#
# Options:
#   -t, --type <TYPE>       Build type: Release (default), Debug, RelWithDebInfo
#   -c, --clean             Clean build (remove build/ first)
#   -j, --jobs <N>          Number of parallel jobs (default: nproc)
#   -T, --test              Run unit tests after build
#   -i, --install <DIR>     Install to directory after build
#   -p, --package           Create a tarball package
#   -v, --verbose           Verbose output
#   -h, --help              Show this help
#
# Examples:
#   ./scripts/build_deploy.sh                          # Release build
#   ./scripts/build_deploy.sh -t Debug -c -T           # Clean debug build + tests
#   ./scripts/build_deploy.sh -t Release -i /opt/engine # Build and install
#   ./scripts/build_deploy.sh -t Release -p             # Build and package

set -euo pipefail

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ---- Defaults ----
BUILD_TYPE="Release"
CLEAN_BUILD=false
NUM_JOBS=$(nproc 2>/dev/null || echo 4)
RUN_TESTS=false
INSTALL_DIR=""
CREATE_PACKAGE=false
VERBOSE=false

# ---- Project paths ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"

# ---- Banner ----
print_banner() {
    echo -e "${CYAN}${BOLD}"
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║     Aspira Trading Engine — Build & Deploy           ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

# ---- Logging helpers ----
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
            -t|--type)
                BUILD_TYPE="$2"
                if [[ ! "$BUILD_TYPE" =~ ^(Release|Debug|RelWithDebInfo)$ ]]; then
                    log_error "Invalid build type: $BUILD_TYPE"
                    log_error "Must be one of: Release, Debug, RelWithDebInfo"
                    exit 1
                fi
                shift 2
                ;;
            -c|--clean)
                CLEAN_BUILD=true
                shift
                ;;
            -j|--jobs)
                NUM_JOBS="$2"
                shift 2
                ;;
            -T|--test)
                RUN_TESTS=true
                shift
                ;;
            -i|--install)
                INSTALL_DIR="$2"
                shift 2
                ;;
            -p|--package)
                CREATE_PACKAGE=true
                shift
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -h|--help)
                usage
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                ;;
        esac
    done
}

# ---- System check ----
check_prerequisites() {
    log_step "Checking prerequisites..."

    local missing=()

    for cmd in cmake make gcc g++; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required tools: ${missing[*]}"
        log_info "Install with: sudo apt install build-essential cmake"
        exit 1
    fi

    # Check CMake version
    local cmake_ver
    cmake_ver=$(cmake --version | head -1 | grep -oP '\d+\.\d+' | head -1)
    if [[ -n "$cmake_ver" ]] && awk "BEGIN {exit !($cmake_ver < 3.16)}"; then
        log_error "CMake >= 3.16 required, found $cmake_ver"
        exit 1
    fi

    # Check GCC version (need >= 10 for C++20)
    local gcc_ver
    gcc_ver=$(gcc -dumpversion | cut -d. -f1)
    if [[ "$gcc_ver" -lt 10 ]]; then
        log_warn "GCC $gcc_ver detected. C++20 support requires GCC >= 10."
    fi

    log_ok "All prerequisites satisfied"
    log_info "  CMake: $(cmake --version | head -1)"
    log_info "  GCC:   $(gcc --version | head -1)"
    log_info "  G++:   $(g++ --version | head -1)"
}

# ---- Clean build ----
do_clean() {
    if [[ "$CLEAN_BUILD" == true ]]; then
        log_step "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        log_ok "Build directory removed: $BUILD_DIR"
    fi
}

# ---- Configure ----
do_configure() {
    log_step "Configuring CMake (${BUILD_TYPE})..."

    mkdir -p "$BUILD_DIR"

    local cmake_opts=(
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    )

    if [[ "$VERBOSE" == true ]]; then
        cmake_opts+=("-DCMAKE_VERBOSE_MAKEFILE=ON")
    fi

    cd "$BUILD_DIR"
    if ! cmake "${cmake_opts[@]}" .. ; then
        log_error "CMake configuration failed"
        exit 1
    fi
    cd "$PROJECT_DIR"

    log_ok "CMake configuration complete"
}

# ---- Build ----
do_build() {
    log_step "Building (${NUM_JOBS} parallel jobs, ${BUILD_TYPE})..."

    local start_time
    start_time=$(date +%s)

    if ! cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j "$NUM_JOBS"; then
        log_error "Build failed"
        exit 1
    fi

    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    log_ok "Build completed in ${elapsed}s"
}

# ---- Test ----
do_test() {
    if [[ "$RUN_TESTS" != true ]]; then
        return
    fi

    log_step "Running unit tests..."

    local passed=0
    local failed=0
    local test_binaries=(
        "${BUILD_DIR}/test_ring_buffer"
        "${BUILD_DIR}/test_memory_pool"
        "${BUILD_DIR}/test_order_book"
    )

    for test_bin in "${test_binaries[@]}"; do
        local test_name
        test_name=$(basename "$test_bin")

        if [[ ! -x "$test_bin" ]]; then
            log_warn "Test binary not found: $test_bin (skipping)"
            continue
        fi

        echo -n "  ${test_name} ... "
        if "$test_bin" > "/tmp/trading_test_${test_name}.log" 2>&1; then
            echo -e "${GREEN}PASS${NC}"
            passed=$((passed + 1))
        else
            echo -e "${RED}FAIL${NC}"
            log_error "Test output:"
            sed 's/^/    /' "/tmp/trading_test_${test_name}.log"
            failed=$((failed + 1))
        fi
    done

    echo ""
    if [[ "$failed" -eq 0 ]]; then
        log_ok "All ${passed} test suites passed"
    else
        log_error "${passed} passed, ${failed} failed"
        exit 1
    fi
}

# ---- Install ----
do_install() {
    if [[ -z "$INSTALL_DIR" ]]; then
        return
    fi

    log_step "Installing to ${INSTALL_DIR}..."

    mkdir -p "$INSTALL_DIR/bin"
    mkdir -p "$INSTALL_DIR/share/aspira-trading"

    cp "${BUILD_DIR}/trading_engine" "${INSTALL_DIR}/bin/"
    cp "${PROJECT_DIR}/docs/high_performance_trading_system.md" "${INSTALL_DIR}/share/aspira-trading/"
    cp "${PROJECT_DIR}/LICENSE" "${INSTALL_DIR}/share/aspira-trading/" 2>/dev/null || true

    log_ok "Installed to $INSTALL_DIR"
    log_info "  Binary: ${INSTALL_DIR}/bin/trading_engine"
}

# ---- Package ----
do_package() {
    if [[ "$CREATE_PACKAGE" != true ]]; then
        return
    fi

    log_step "Creating deployment package..."

    local pkg_dir="${PROJECT_DIR}/aspira-trading-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "${pkg_dir}/bin"
    mkdir -p "${pkg_dir}/docs"
    mkdir -p "${pkg_dir}/scripts"

    cp "${BUILD_DIR}/trading_engine" "${pkg_dir}/bin/"
    cp "${PROJECT_DIR}/docs/high_performance_trading_system.md" "${pkg_dir}/docs/"
    cp "${PROJECT_DIR}/scripts/build_deploy.sh" "${pkg_dir}/scripts/"

    # Create a run script
    cat > "${pkg_dir}/run.sh" << 'RUNEOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${DIR}/bin/trading_engine" "$@"
RUNEOF
    chmod +x "${pkg_dir}/run.sh"

    local tarball="${pkg_dir}.tar.gz"
    tar -czf "$tarball" -C "$(dirname "$pkg_dir")" "$(basename "$pkg_dir")"
    rm -rf "$pkg_dir"

    local size
    size=$(du -h "$tarball" | cut -f1)

    log_ok "Package created: $tarball (${size})"
}

# ---- Summary ----
print_summary() {
    echo ""
    echo -e "${GREEN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}${BOLD}║              Build & Deploy — Complete               ║${NC}"
    echo -e "${GREEN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  Build type:    ${BOLD}${BUILD_TYPE}${NC}"
    echo -e "  Build dir:     ${BUILD_DIR}"
    echo -e "  Binary:        ${BUILD_DIR}/trading_engine"
    echo -e "  Binary size:   $(du -h "${BUILD_DIR}/trading_engine" 2>/dev/null | cut -f1)"
    echo ""

    if [[ "$RUN_TESTS" == true ]]; then
        echo -e "  Tests:         ${GREEN}All passed${NC}"
    fi

    if [[ -n "$INSTALL_DIR" ]]; then
        echo -e "  Installed to:  ${INSTALL_DIR}"
    fi

    echo ""
    echo -e "  Run with:"
    echo -e "    ${CYAN}${BUILD_DIR}/trading_engine --duration 10 --symbol AAPL${NC}"
    echo ""
}

# ---- Main ----
main() {
    print_banner
    parse_args "$@"

    echo -e "  Build type:    ${BOLD}${BUILD_TYPE}${NC}"
    echo -e "  Clean build:   ${CLEAN_BUILD}"
    echo -e "  Parallel jobs: ${NUM_JOBS}"
    echo -e "  Run tests:     ${RUN_TESTS}"
    echo -e "  Install dir:   ${INSTALL_DIR:-<none>}"
    echo -e "  Package:       ${CREATE_PACKAGE}"
    echo ""

    check_prerequisites
    do_clean
    do_configure
    do_build
    do_test
    do_install
    do_package
    print_summary
}

main "$@"
