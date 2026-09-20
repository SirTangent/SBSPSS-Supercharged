#!/bin/bash
# Configure + build the PC (Win32) port: every preset by default.
#
#   port/build-pc.sh [<preset>|usa|eur|clangcl|clangcl64|all] [extra ninja args...]
#   port/build-pc.sh test [usa|eur|clangcl|clangcl64]  build, then ctest -L unit and -L playthrough on each tree
#   port/build-pc.sh soak [usa|eur|clangcl|clangcl64]  build, then the full Tier 1 + Tier 2 sweep on each tree
#   port/build-pc.sh parity64 [debug|final]  build clang-cl x86 + x64, then the x64 A/B (streams, cross replay, cards)
#
# Presets (port/CMakePresets.json): debug, final (USA; usa-debug / usa-final
# are accepted aliases) and eur-debug, eur-final (EUR, PAL 50Hz); `usa` /
# `eur` name a territory's pair and `all` is every MinGW tree.  Each tree
# needs its territory's data first (one build serves DEBUG and FINAL, issue
# #35):
#   port/build-data.cmd usa            (out/USA/include + out/USA/cd)
#   port/build-data.cmd eur            (out/EUR/include + out/EUR/cd)
# - build_one checks for both and names the missing command.
#
# clangcl-debug / clangcl-final (`clangcl` = both) are the same USA trees
# built by clang-cl against the MSVC CRT (cmake/clangcl-toolchain.cmake:
# needs LLVM + Visual Studio's x86 build tools, not MSYS2 - only its ninja
# is borrowed).  They are kept out of `all`: the MinGW exes are the ones
# that ship.
#
# clangcl-x64-debug / clangcl-x64-final (`clangcl64` = both) are clang-cl
# again, targeting x86_64 (M9); also kept out of `all`.
#
# SBSP_CODEVIEW=1 in the environment configures the MinGW trees with
# -DSBSP_CODEVIEW=ON (a .pdb beside every exe, for Visual Studio / WinDbg).
#
# Requires the MSYS2 mingw32 toolchain:
#   pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja

set -e
# presets live in port/, and cmake resolves --preset from the cwd
cd "$(dirname "$0")"

# cc1plus/ninja need the mingw32 runtime DLLs on PATH.
# Override MSYS2_ROOT (POSIX-style path) for a non-default MSYS2 install.
MSYS2_ROOT="${MSYS2_ROOT:-/c/msys64}"
export PATH="$MSYS2_ROOT/mingw32/bin:$PATH"

what="${1:-all}"
shift 2>/dev/null || true

usage()
{
    echo "usage: port/build-pc.sh [debug|final|usa-debug|usa-final|eur-debug|eur-final|clangcl-debug|clangcl-final|clangcl-x64-debug|clangcl-x64-final|usa|eur|clangcl|clangcl64|all|test [usa|eur|clangcl|clangcl64]|soak [usa|eur|clangcl|clangcl64]|parity64 [debug|final]] [ninja args]" >&2
    exit 1
}

# the preset list a territory word (or `all`) stands for
presets_for()
{
    w=$(echo "$1" | tr '[:upper:]' '[:lower:]')     # any case, like build-data.sh
    case "$w" in
        usa)  echo "debug final" ;;
        eur)  echo "eur-debug eur-final" ;;
        clangcl) echo "clangcl-debug clangcl-final" ;;
        clangcl64) echo "clangcl-x64-debug clangcl-x64-final" ;;
        all|"") echo "debug final eur-debug eur-final" ;;
        debug|final|eur-debug|eur-final|clangcl-debug|clangcl-final|clangcl-x64-debug|clangcl-x64-final) echo "$w" ;;
        usa-debug) echo "debug" ;;      # the build-data.sh spelling, same tree
        usa-final) echo "final" ;;
        *) usage ;;
    esac
}

# The data a preset's exe compiles against and boots from.  CMake only gates
# on the generated headers; out/<T>/cd is what cd.cpp opens at run time (the
# same files for DEBUG and FINAL), so check both here and say what to run.
check_data()
{
    preset="$1"
    case "$preset" in
        eur-*) terr=EUR ;;
        *)     terr=USA ;;
    esac
    for f in "../out/$terr/include/BigLump.h" "../out/$terr/cd/BIGLUMP.BIN"; do
        if [ ! -f "$f" ]; then
            echo "missing $f - run: port/build-data.cmd $(echo "$terr" | tr '[:upper:]' '[:lower:]')" >&2
            exit 1
        fi
    done
}

build_one()
{
    preset="$1"
    shift                       # the rest is extra ninja args, not the preset
    check_data "$preset"
    echo "=== configure+build: $preset ==="
    case "$preset" in
        clangcl-*) cmake --preset "$preset" ;;
        *)         cmake --preset "$preset" -DSBSP_CODEVIEW="${SBSP_CODEVIEW:-0}" ;;
    esac
    cmake --build --preset "$preset" "$@"
}

test_one()
{
    preset="$1"
    echo "=== ctest ($preset): unit ==="
    ctest --test-dir "build/$preset" --output-on-failure -L unit --no-tests=error
    # --no-tests=error turns "the label matched nothing" into a failure, which
    # is the point: a playthrough label that quietly registered zero tests used
    # to read green.  A shim-only tree (-DSBSP_BUILD_GAME=OFF) legitimately has
    # no playthrough tests, though, so say so and skip rather than fail - the
    # strict form is what CI runs, and CI always builds the game.
    if [ ! -f "build/$preset/sbsp.exe" ]; then
        echo "=== ctest ($preset): playthrough SKIPPED - no sbsp.exe (SBSP_BUILD_GAME=OFF?) ==="
        return
    fi
    echo "=== ctest ($preset): playthrough ==="
    ctest --test-dir "build/$preset" --output-on-failure -L playthrough --no-tests=error
}

soak_one()
{
    preset="$1"
    case "$preset" in
        eur-*) terr=EUR ;;
        *)     terr=USA ;;
    esac
    echo "=== soak ($preset): tier 1 + tier 2, all routes / all levels ==="
    python3 tests/run_tier.py --exe "build/$preset/sbsp.exe" --territory "$terr" \
        --selftest --tier1 --tier2 --logs "build/$preset/soak-logs"
}

# The x64 A/B (M9): the 32-bit clang-cl exe plays every Tier 1 route and
# Tier 2 level and keeps its logs, recordings and memory cards; the x64 exe
# must then (1) produce the same [scene]/[frame] streams playing the same
# tiers itself and (2) replay the 32-bit recordings without a desync, to the
# same streams and byte-identical cards.
#
# Every leg runs even if an earlier one fails, and the combined result is the
# exit status: leg 3 is the only thing that exercises the cross-exe replay and
# the card compare, so a known divergence in leg 2 - FINAL has one, see
# conv_pc.md - must not be what stops it from ever being reached.
parity64()
{
    v="$1"
    d="build/parity64-$v"
    rc=0
    rm -rf "$d"
    echo "=== parity64 ($v): x86 baseline ==="
    # --no-replay: the baseline's own determinism replay is already a ctest,
    # and the recording leg 3 needs is written by the first pass regardless.
    python3 tests/run_tier.py --exe "build/clangcl-$v/sbsp.exe" --tier1 --tier2 \
        --no-replay --logs "$d/L32" --keep-artifacts "$d/A32" || rc=1
    echo "=== parity64 ($v): x64, same tiers ==="
    python3 tests/run_tier.py --exe "build/clangcl-x64-$v/sbsp.exe" --tier1 --tier2 \
        --logs "$d/L64" --compare-frames "$d/L32" || rc=1
    echo "=== parity64 ($v): x64 replays the x86 recordings ==="
    python3 tests/run_tier.py --exe "build/clangcl-x64-$v/sbsp.exe" --tier1 \
        --logs "$d/L64x" --compare-frames "$d/L32" --replay-from "$d/A32" || rc=1
    return $rc
}

# presets_for runs in a command substitution, so its usage exit must be
# re-checked here or a bad word would silently build nothing.
case "$what" in
    test)
        list=$(presets_for "${1:-all}") || exit 1
        for p in $list; do build_one "$p"; done
        for p in $list; do test_one "$p"; done
        ;;
    soak)
        list=$(presets_for "${1:-all}") || exit 1
        for p in $list; do build_one "$p"; done
        for p in $list; do soak_one "$p"; done
        ;;
    parity64)
        # DEBUG by default: it is the variant the A/B is green on.  FINAL has
        # a known 39-frame divergence (campaign 6858-6897, a #39 gate-2
        # matter), so `parity64 final` is expected to report a failure.
        v=$(echo "${1:-debug}" | tr '[:upper:]' '[:lower:]')
        case "$v" in debug|final) ;; *) usage ;; esac
        build_one "clangcl-$v"
        build_one "clangcl-x64-$v"
        parity64 "$v"
        ;;
    *)
        list=$(presets_for "$what") || exit 1
        for p in $list; do build_one "$p" "$@"; done
        ;;
esac

echo "PC build complete."
