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
JOBS=$(nproc)

# ── usage ──────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $0 [release|debug|clean] [flags]

Modes:
  release      Production build (default)
  debug        Debug build with ASan/UBSan
  clean        Remove build/ directories

Flags:
  --tests      Enable BUILD_TESTS=ON
  --bench      Enable BUILD_BENCHMARKS=ON
  --no-conan   Skip conan install (use cached conan output)
  --quick      Skip conan install + cmake configure (fast incremental rebuild)
  --bin        Build only the trade_bot binary (skip tests, labeler, etc.)
  --jobs N     Override parallel jobs (default: nproc)
EOF
    exit 0
}

# ── parse args ─────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage ;;
        release|debug) MODE="$1"; shift ;;
        clean) MODE="clean"; shift ;;
        --tests) BUILD_TESTS="ON"; shift ;;
        --bench) BUILD_BENCHMARKS="ON"; shift ;;
        --no-conan) NO_CONAN=true; shift ;;
        --quick) QUICK=true; NO_CONAN=true; shift ;;
        --bin) BIN_ONLY=true; BUILD_TESTS="OFF"; BUILD_BENCHMARKS="OFF"; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# ── colors ─────────────────────────────────────────────────
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

step()   { echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} ${BOLD}$1${NC}"; }
ok()     { echo -e "  ${GREEN}✔${NC} $1"; }
warn()   { echo -e "  ${YELLOW}⚠${NC} $1"; }
die()    { echo -e "  ${RED}✘${NC} $1"; exit 1; }

# ── clean ──────────────────────────────────────────────────
if [ "$MODE" = "clean" ]; then
    step "Cleaning build directories ..."
    rm -rf build/release build/debug
    ok "Clean complete"
    exit 0
fi

# ── derived vars ───────────────────────────────────────────
BUILD_DIR="build/$MODE"
case "$MODE" in
    release) BUILD_TYPE="Release" ;;
    debug)   BUILD_TYPE="Debug" ;;
esac

START_TIME=$SECONDS
echo -e "${BOLD}═══ ${GREEN}Build: $MODE${NC} ${BOLD}═══${NC}"

# ── conan install ─────────────────────────────────────────
if [ "$NO_CONAN" = false ]; then
    step "Conan install ($MODE) ..."
    if [ "$MODE" = "debug" ]; then
        conan install core \
            --output-folder="$BUILD_DIR" \
            --build=missing \
            -pr debug-asan \
            || die "Conan install failed"
    else
        conan install core \
            --output-folder="$BUILD_DIR" \
            --build=missing \
            -s build_type="$BUILD_TYPE" \
            || die "Conan install failed"
    fi
    ok "Conan install done"
else
    warn "Skipping conan install"
fi

# ── cmake configure ───────────────────────────────────────
if [ "$QUICK" = false ]; then
    step "CMake configure ($MODE) ..."
    cmake -S core --preset "$MODE" \
        -DBUILD_TESTS="$BUILD_TESTS" \
        -DBUILD_BENCHMARKS="$BUILD_BENCHMARKS" \
        || die "CMake configure failed"
    ok "CMake configure done"
else
    warn "Skipping cmake configure (--quick)"
fi

# ── cmake build ───────────────────────────────────────────
CMAKE_TARGET=""
if [ "$BIN_ONLY" = true ]; then
    CMAKE_TARGET="--target trade_bot"
    step "CMake build trade_bot only (-j$JOBS) ..."
else
    step "CMake build (-j$JOBS) ..."
fi
cmake --build --preset "$MODE" -j"$JOBS" $CMAKE_TARGET || die "Build failed"
ok "Build complete"

# ── summary ───────────────────────────────────────────────
ELAPSED=$((SECONDS - START_TIME))
echo ""
echo -e "${GREEN}${BOLD}✓ Done in ${ELAPSED}s${NC}"
echo -e "  Binary: ${BOLD}${BUILD_DIR}/bin/trade_bot${NC}"
