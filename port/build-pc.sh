#!/bin/bash
# Configure + build the PC (Win32) port: both DEBUG and FINAL by default.
#
#   port/build-pc.sh [debug|final|all] [extra ninja args...]
#   port/build-pc.sh test        build both, then ctest -L unit and -L playthrough on each
#   port/build-pc.sh soak        build both, then the full Tier 1 + Tier 2 sweep on each
#
# Requires the MSYS2 mingw32 toolchain:
#   pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja
# and the M0 generated headers (run port/build-data.cmd first).

set -e
# presets live in port/, and cmake resolves --preset from the cwd
cd "$(dirname "$0")"

# cc1plus/ninja need the mingw32 runtime DLLs on PATH.
# Override MSYS2_ROOT (POSIX-style path) for a non-default MSYS2 install.
MSYS2_ROOT="${MSYS2_ROOT:-/c/msys64}"
export PATH="$MSYS2_ROOT/mingw32/bin:$PATH"

what="${1:-all}"
shift 2>/dev/null || true

build_one()
{
    preset="$1"
    shift                       # the rest is extra ninja args, not the preset
    echo "=== configure+build: $preset ==="
    cmake --preset "$preset"
    cmake --build --preset "$preset" "$@"
}

test_one()
{
    preset="$1"
    echo "=== ctest ($preset): unit ==="
    ctest --test-dir "build/$preset" --output-on-failure -L unit
    echo "=== ctest ($preset): playthrough ==="
    ctest --test-dir "build/$preset" --output-on-failure -L playthrough
}

soak_one()
{
    preset="$1"
    echo "=== soak ($preset): tier 1 + tier 2, all routes / all levels ==="
    python3 tests/run_tier.py --exe "build/$preset/sbsp.exe" --selftest --tier1 --tier2 \
        --logs "build/$preset/soak-logs"
}

case "$what" in
    debug|final) build_one "$what" "$@" ;;
    all)         build_one debug "$@"; build_one final "$@" ;;
    test)        build_one debug; build_one final; test_one debug; test_one final ;;
    soak)        build_one debug; build_one final; soak_one debug; soak_one final ;;
    *) echo "usage: port/build-pc.sh [debug|final|all|test|soak] [ninja args]" >&2; exit 1 ;;
esac

echo "PC build complete."
