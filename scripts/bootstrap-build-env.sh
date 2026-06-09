#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# bootstrap-build-env.sh - Idempotent build-environment bootstrap for local
# worktrees and CI runners.
#
# Guarantees after successful exit:
#   1. cmake, ninja, conan (2.x), and a supported C++ compiler are on PATH.
#   2. Conan default profile exists and is usable for `conan install core`.
#   3. Conan remote "conancenter" points at https://center2.conan.io
#      (legacy center.conan.io is removed/disabled to avoid 403 errors).
#
# Safe to run repeatedly - every step is guarded by an idempotency check.
#
# External tool evidence (see FN-024 PROMPT.md § External Integration Evidence):
#   Conan:       https://github.com/conan-io/conan   | PyPI: conan
#   Conan Center: https://center2.conan.io            | conan remote
#   Ninja:       https://github.com/ninja-build/ninja | apt: ninja-build
#   CMake:       https://cmake.org/download/          | apt: cmake
#   GCC:         https://gcc.gnu.org/                 | apt: g++
#   Clang:       https://llvm.org/docs/               | apt: clang++
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── colors ───────────────────────────────────────────────────────────────────
GREEN=$'\033[0;32m'
BLUE=$'\033[0;34m'
YELLOW=$'\033[1;33m'
RED=$'\033[0;31m'
DIM=$'\033[2m'
BOLD=$'\033[1m'
NC=$'\033[0m'

step()  { echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} ${BOLD}$*${NC}"; }
ok()    { echo -e "  ${GREEN}✔${NC} $*"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
die()   { echo -e "\n  ${RED}✘ $*${NC}"; exit 1; }
info()  { echo -e "  ${DIM}$*${NC}"; }

# ── header ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║${NC}  ${GREEN}${BOLD}trade-bot build-environment bootstrap${NC}   ${BOLD}║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

# ── 1. Validate required host tools ─────────────────────────────────────────
REQUIRED_TOOLS=(
    "cmake:CMake 3.22+ - https://cmake.org/download/ | apt: cmake"
    "ninja:Ninja 1.10+ - https://github.com/ninja-build/ninja/releases | apt: ninja-build"
    "conan:Conan 2.x - https://pypi.org/project/conan/ | pip: conan"
)

MISSING_TOOLS=()

for entry in "${REQUIRED_TOOLS[@]}"; do
    tool="${entry%%:*}"
    hint="${entry#*:}"
    if command -v "$tool" >/dev/null 2>&1; then
        ok "$tool found: $(command -v "$tool")"
    else
        MISSING_TOOLS+=("$hint")
    fi
done

# Check for a supported C++ compiler (GCC 14+ or Clang 17+)
COMPILER=""
COMPILER_VERSION=""
if command -v g++ >/dev/null 2>&1; then
    COMPILER="g++"
    # Extract major version from g++ output (e.g., "g++ (GCC) 14.2.0" → 14)
    COMPILER_VERSION=$(g++ -dumpversion 2>/dev/null | cut -d. -f1)
    ok "g++ found: $(g++ --version 2>&1 | head -1)"
elif command -v clang++ >/dev/null 2>&1; then
    COMPILER="clang++"
    # Extract major version from clang++ output (e.g., "clang version 17.0.6" → 17)
    COMPILER_VERSION=$(clang++ --version 2>&1 | grep -oP 'version \K[0-9]+' | head -1)
    ok "clang++ found: $(clang++ --version 2>&1 | head -1)"
else
    MISSING_TOOLS+=("g++ or clang++:GCC 14+ or Clang 17+ - https://gcc.gnu.org/ | apt: g++")
fi

if [ ${#MISSING_TOOLS[@]} -gt 0 ]; then
    echo ""
    echo -e "${RED}${BOLD}Missing required tools:${NC}"
    for hint in "${MISSING_TOOLS[@]}"; do
        echo -e "  ${RED}✘${NC} $hint"
    done
    echo ""
    echo -e "${YELLOW}Install and re-run this script.${NC}"
    exit 1
fi

# ── 2. Proxy configuration for Conan remote access ────────────────────────
# External integration evidence: Conan requires proxy access to center2.conan.io
# in some environments. Configure via CONAN_PROXY_URL or HTTP_PROXY/HTTPS_PROXY.
# canonical-upstream-repo-url: https://github.com/conan-io/conan
# release-or-download-url: https://pypi.org/project/conan/
# binary-or-cli-name: conan
# checksum-or-source-of-truth-evidence: PyPI package metadata
CONAN_PROXY_URL="${CONAN_PROXY_URL:-}"
if [ -n "$CONAN_PROXY_URL" ]; then
    step "Configuring proxy for Conan …"
    export HTTP_PROXY="$CONAN_PROXY_URL"
    export HTTPS_PROXY="$CONAN_PROXY_URL"
    ok "Proxy configured: $CONAN_PROXY_URL"
elif [ -n "${HTTP_PROXY:-}" ] || [ -n "${HTTPS_PROXY:-}" ]; then
    info "Using existing proxy from environment: HTTP_PROXY=${HTTP_PROXY:-<unset>} HTTPS_PROXY=${HTTPS_PROXY:-<unset>}"
fi

# ── 3. Verify Conan major version is 2.x ────────────────────────────────────
step "Checking Conan version …"
CONAN_VERSION_OUTPUT=$(conan --version 2>&1)
# Expected format: "Conan version 2.29.0"
CONAN_MAJOR=$(echo "$CONAN_VERSION_OUTPUT" | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' | head -1 | cut -d. -f1)

if [ -z "$CONAN_MAJOR" ]; then
    die "Could not parse Conan version from: $CONAN_VERSION_OUTPUT"
fi

if [ "$CONAN_MAJOR" -lt 2 ]; then
    die "Conan 2.x is required but found Conan $CONAN_VERSION_OUTPUT. Upgrade with: pip install --upgrade 'conan>=2,<3'"
fi

ok "Conan $CONAN_VERSION_OUTPUT major=$CONAN_MAJOR"

# ── 4. Verify compiler version meets minimum ────────────────────────────────
step "Checking compiler version ..."
if [ "$COMPILER" = "g++" ] && [ "${COMPILER_VERSION:-0}" -lt 14 ]; then
    die "GCC 14+ is required but found GCC $COMPILER_VERSION. Install GCC 14+: https://gcc.gnu.org/"
elif [ "$COMPILER" = "clang++" ] && [ "${COMPILER_VERSION:-0}" -lt 17 ]; then
    die "Clang 17+ is required but found Clang $COMPILER_VERSION. Install Clang 17+: https://llvm.org/docs/"
fi
ok "$COMPILER version $COMPILER_VERSION meets minimum requirement"

# ── 5. Ensure Conan default profile exists ──────────────────────────────────
step "Checking Conan default profile ..."
# Conan 2.x uses 'conan profile show' (without 'default') to show the default profile
if conan profile show >/dev/null 2>&1; then
    ok "Conan default profile already exists"
    info "$(conan profile show 2>&1 | head -3 | tr '\n' ' ')"
else
    info "No default profile found - running 'conan profile detect --force'"
    conan profile detect --force 2>&1
    ok "Conan default profile created"
fi

# ── 6. Configure Conan remotes ──────────────────────────────────────────────
# Ensure "conancenter" points to https://center2.conan.io (Conan 2 endpoint).
# The legacy https://center.conan.io endpoint returns 403 for Conan 2 clients.
step "Checking Conan remotes ..."

CORRECT_URL="https://center2.conan.io"
REMOTE_NAME="conancenter"

# Get current remote URL if it exists
CURRENT_URL=$(conan remote list 2>&1 | grep "^${REMOTE_NAME}:" | awk '{print $2}' || true)

if [ "$CURRENT_URL" = "$CORRECT_URL" ]; then
    ok "Conan remote '$REMOTE_NAME' already configured correctly"
else
    if [ -n "$CURRENT_URL" ]; then
        info "Updating remote '$REMOTE_NAME' from $CURRENT_URL → $CORRECT_URL"
        conan remote update "$REMOTE_NAME" "$CORRECT_URL" --index 0 2>&1
    else
        info "Adding remote '$REMOTE_NAME' → $CORRECT_URL"
        conan remote add "$REMOTE_NAME" "$CORRECT_URL" --index 0 2>&1
    fi
    ok "Conan remote '$REMOTE_NAME' → $CORRECT_URL"
fi

# ── 7. Verify CMake can find Ninja generator ───────────────────────────────
step "Verifying Ninja generator availability ..."
# Note: grep -q causes SIGPIPE with pipefail, so use grep > /dev/null instead
if cmake --help 2>&1 | grep Ninja > /dev/null; then
    ok "CMake Ninja generator available"
else
    die "CMake Ninja generator not found. Install Ninja: https://github.com/ninja-build/ninja/releases | apt: ninja-build"
fi

# ── done ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}✔ Build environment bootstrap complete.${NC}"
echo -e "  ${DIM}You can now run: ./scripts/build.sh debug --tests${NC}"
echo ""
