#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# ── defaults ───────────────────────────────────────────────
MODE="release"
BUILD_TESTS="OFF"
BUILD_BENCHMARKS="OFF"
NO_CONAN=false
QUICK=false
BIN_ONLY=false
VERBOSE=false
JOBS=$(nproc)
TARGET=""

# ── colors ─────────────────────────────────────────────────
GREEN=$'\033[0;32m'
BLUE=$'\033[0;34m'
CYAN=$'\033[0;36m'
YELLOW=$'\033[1;33m'
RED=$'\033[0;31m'
DIM=$'\033[2m'
BOLD=$'\033[1m'
NC=$'\033[0m'

step()   { echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} ${BOLD}$*${NC}"; }
ok()     { echo -e "  ${GREEN}✔${NC} $*"; }
warn()   { echo -e "  ${YELLOW}⚠${NC} $*"; }
die()    { echo -e "\n  ${RED}✘ $*${NC}"; exit 1; }
info()   { echo -e "  ${DIM}$*${NC}"; }

# ── usage ──────────────────────────────────────────────────
usage() {
    cat <<EOF

${BOLD}Usage:${NC}
  $0 [mode] [flags]

${BOLD}Modes:${NC}
  ${GREEN}release${NC}      Production build, O3 + LTO  (default)
  ${GREEN}debug${NC}        Debug build with ASan/UBSan
  ${GREEN}clean${NC}        Remove build directories

${BOLD}What to build:${NC}
  ${CYAN}--bench${NC}      Include benchmark binaries
  ${CYAN}--tests${NC}      Include unit/integration tests
  ${CYAN}--all${NC}        Include both benchmarks and tests
  ${CYAN}--bin${NC}        Build only trade_bot  (fastest)
  ${CYAN}--target X${NC}   Build a single named target  (e.g. perf_replay)

${BOLD}Speed:${NC}
  ${CYAN}--quick${NC}      Skip conan + cmake configure  (incremental recompile)
  ${CYAN}--no-conan${NC}   Skip conan install only
  ${CYAN}--jobs N${NC}     Parallel jobs  (default: nproc = ${JOBS})

${BOLD}Output:${NC}
  ${CYAN}-v, --verbose${NC}  Verbose cmake build output

${BOLD}Examples:${NC}
  $0                        # release build, main binaries
  $0 --bench                # release + all bench_* binaries
  $0 --all                  # release + tests + bench
  $0 --quick                # fast recompile, no reconfigure
  $0 --quick --bench        # fast recompile + bench targets
  $0 --target perf_replay   # build only perf_replay
  $0 debug --tests          # debug build with tests
  $0 clean                  # remove build/release and build/debug
  $0 clean debug            # remove only build/debug

EOF
    exit 0
}

# ── parse args ─────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)    usage ;;
        release|debug) MODE="$1"; shift ;;
        clean)        MODE="clean"; shift ;;
        --tests)      BUILD_TESTS="ON"; shift ;;
        --bench)      BUILD_BENCHMARKS="ON"; shift ;;
        --all)        BUILD_TESTS="ON"; BUILD_BENCHMARKS="ON"; shift ;;
        --no-conan)   NO_CONAN=true; shift ;;
        --quick)      QUICK=true; NO_CONAN=true; shift ;;
        --bin)        BIN_ONLY=true; BUILD_TESTS="OFF"; BUILD_BENCHMARKS="OFF"; shift ;;
        --target)     TARGET="$2"; shift 2 ;;
        --jobs|-j)    JOBS="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=true; shift ;;
        *) echo -e "${RED}Unknown option: $1${NC}"; usage ;;
    esac
done

# ── clean ──────────────────────────────────────────────────
if [ "$MODE" = "clean" ]; then
    # "clean debug" → only debug; "clean" → both
    if [[ "${1:-}" == "release" || "${1:-}" == "debug" ]]; then
        CLEAN_DIR="build/${1}"
        step "Removing $CLEAN_DIR ..."
        rm -rf "$CLEAN_DIR"
        ok "Removed $CLEAN_DIR"
    else
        step "Removing build/release and build/debug ..."
        rm -rf build/release build/debug
        ok "Clean complete"
    fi
    exit 0
fi

# ── derived vars ───────────────────────────────────────────
BUILD_DIR="build/$MODE"
case "$MODE" in
    release) BUILD_TYPE="Release" ;;
    debug)   BUILD_TYPE="Debug" ;;
esac

STAGE_START=$SECONDS
START_TIME=$SECONDS

# ── header ─────────────────────────────────────────────────
echo ""
echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
echo -e "${BOLD}║${NC}  ${GREEN}${BOLD}trade-bot build${NC}                      ${BOLD}║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
echo -e "  mode:   ${BOLD}$MODE${NC}"
if [ -n "$TARGET" ]; then
    echo -e "  target: ${BOLD}$TARGET${NC}"
elif [ "$BIN_ONLY" = true ]; then
    echo -e "  target: ${BOLD}trade_bot only${NC}"
else
    extra=""
    [ "$BUILD_TESTS" = "ON" ]      && extra+=" tests"
    [ "$BUILD_BENCHMARKS" = "ON" ] && extra+=" bench"
    [ -n "$extra" ] && echo -e "  extras:${BOLD}${extra}${NC}"
fi
echo -e "  jobs:   ${BOLD}$JOBS${NC}"
echo ""

# ── conan install ──────────────────────────────────────────
if [ "$NO_CONAN" = false ]; then
    STAGE_START=$SECONDS
    step "Conan install …"
    if [ "$MODE" = "debug" ]; then
        conan install core \
            --output-folder="$BUILD_DIR" \
            --build=missing \
            -pr debug-asan \
            2>&1 | grep -E "^(ERROR|WARN|Requirement|Install)" || true
    else
        conan install core \
            --output-folder="$BUILD_DIR" \
            --build=missing \
            -s build_type="$BUILD_TYPE" \
            2>&1 | grep -E "^(ERROR|WARN|Requirement|Install)" || true
    fi
    ok "Conan done  $(( SECONDS - STAGE_START ))s"
else
    warn "Skipping conan install (--no-conan / --quick)"
fi

# ── quick guard: warn if no CMakeCache ────────────────────
if [ "$QUICK" = true ] && [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    warn "--quick: no CMakeCache found in $BUILD_DIR — running configure anyway"
    QUICK=false
fi

# ── cmake configure ────────────────────────────────────────
if [ "$QUICK" = false ]; then
    STAGE_START=$SECONDS
    step "CMake configure ($MODE) …"
    cmake -S core --preset "$MODE" \
        -DBUILD_TESTS="$BUILD_TESTS" \
        -DBUILD_BENCHMARKS="$BUILD_BENCHMARKS" \
        || die "CMake configure failed"
    ok "Configure done  $(( SECONDS - STAGE_START ))s"
else
    warn "Skipping cmake configure (--quick)"
fi

# ── cmake build ────────────────────────────────────────────
STAGE_START=$SECONDS
CMAKE_ARGS=(-j "$JOBS")
[ "$VERBOSE" = true ] && CMAKE_ARGS+=(--verbose)

if [ -n "$TARGET" ]; then
    step "Building target: ${BOLD}$TARGET${NC} (-j$JOBS) …"
    CMAKE_ARGS+=(--target "$TARGET")
elif [ "$BIN_ONLY" = true ]; then
    step "Building trade_bot only (-j$JOBS) …"
    CMAKE_ARGS+=(--target trade_bot)
else
    step "Building all targets (-j$JOBS) …"
fi

cmake --build "$BUILD_DIR" "${CMAKE_ARGS[@]}" || die "Build failed"
ok "Build done  $(( SECONDS - STAGE_START ))s"

# ── summary ────────────────────────────────────────────────
ELAPSED=$((SECONDS - START_TIME))
echo ""
echo -e "${GREEN}${BOLD}✓ Finished in ${ELAPSED}s${NC}  ${DIM}→ $BUILD_DIR/bin/${NC}"
echo ""

show_bin() {
    local name="$1"
    local label="${2:-$1}"
    local f="$BUILD_DIR/bin/$name"
    [ -f "$f" ] || return 0
    local sz
    sz=$(du -h "$f" | cut -f1)
    printf "  ${GREEN}▸${NC} ${BOLD}%-34s${NC} ${YELLOW}%s${NC}\n" "$label" "$sz"
}

show_bin trade_bot   "trade_bot"
show_bin labeler     "labeler"
show_bin perf_replay "perf_replay   (offline profiler)"
show_bin core_probe  "core_probe    (pipeline tracer + AI probe)"

if [ "$BUILD_BENCHMARKS" = "ON" ]; then
    echo ""
    echo -e "  ${CYAN}Benchmarks:${NC}"
    show_bin bench_feature_extractor   "bench_feature_extractor"
    show_bin bench_density_detector    "bench_density_detector"
    show_bin bench_strategy_engine_tick "bench_strategy_engine_tick"
    show_bin bench_end_to_end_pipeline  "bench_end_to_end_pipeline"
    echo ""
    echo -e "  ${DIM}Run:  $BUILD_DIR/bin/bench_end_to_end_pipeline --benchmark_format=console${NC}"
fi

if [ "$BUILD_TESTS" = "ON" ]; then
    echo ""
    echo -e "  ${DIM}Test: cd $BUILD_DIR && ctest --output-on-failure -j$JOBS${NC}"
fi

echo ""
