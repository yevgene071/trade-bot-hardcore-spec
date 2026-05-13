#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# ── defaults ───────────────────────────────────────────────
MODE="debug"
FILTER=""

# ── usage ──────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $0 [debug|release] [--filter PATTERN]

Modes:
  debug      Run tests from build/debug (default)
  release    Run tests from build/release

Flags:
  --filter PATTERN   Run only tests matching regex (passed to ctest -R)
EOF
    exit 0
}

# ── parse args ─────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage ;;
        debug|release) MODE="$1"; shift ;;
        --filter) FILTER="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# ── derived vars ───────────────────────────────────────────
BUILD_DIR="build/$MODE"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: build directory '$BUILD_DIR' not found."
    echo "Run './scripts/build.sh $MODE --tests' first."
    exit 1
fi

# ── colors ─────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

# ── run tests ──────────────────────────────────────────────
echo -e "${BOLD}═══ Tests: $MODE${NC}"
echo ""

CTEST_ARGS=(
    --test-dir "$BUILD_DIR"
    --output-on-failure
)

if [ -n "$FILTER" ]; then
    echo "Filter: $FILTER"
    CTEST_ARGS+=(-R "$FILTER")
fi

# Run ctest and capture output for summary parsing
TMPFILE=$(mktemp)
set +e
ctest "${CTEST_ARGS[@]}" 2>&1 | tee "$TMPFILE"
CTEST_EXIT=$?
set -e

# ── summary ────────────────────────────────────────────────
echo ""
echo -e "${BOLD}─── Summary ───${NC}"

# Extract ctest summary line (e.g. "100% tests passed, 0 tests failed out of 50")
SUMMARY=$(grep -E "^[0-9]+% tests passed" "$TMPFILE" | tail -1 || true)

if [ -n "$SUMMARY" ]; then
    if echo "$SUMMARY" | grep -q "100%"; then
        echo -e "  ${GREEN}${BOLD}${SUMMARY}${NC}"
    else
        echo -e "  ${RED}${BOLD}${SUMMARY}${NC}"
    fi
fi

rm -f "$TMPFILE"
exit $CTEST_EXIT
