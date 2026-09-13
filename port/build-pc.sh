#!/bin/bash
# Configure + build the PC (Win32) port: every preset by default.
#
#   port/build-pc.sh [<preset>|usa|eur|all] [extra ninja args...]
#   port/build-pc.sh test [usa|eur]    build, then ctest -L unit and -L playthrough on each tree
#   port/build-pc.sh soak [usa|eur]    build, then the full Tier 1 + Tier 2 sweep on each tree
#
# Presets (port/CMakePresets.json): debug, final (USA; usa-debug / usa-final
# are accepted aliases) and eur-debug, eur-final (EUR, PAL 50Hz); `usa` /
# `eur` name a territory's pair and `all` is every tree.  Each tree needs
# its territory+variant data first, built with the SAME word:
#   port/build-data.cmd final          (= USA FINAL: out/USA/include + out/USA/FINAL/version/CD)
#   port/build-data.cmd eur-debug      (= EUR DEBUG: out/EUR/include + out/EUR/DEBUG/version/CD)
# - build_one checks for both and names the missing command.
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
    echo "usage: port/build-pc.sh [debug|final|usa-debug|usa-final|eur-debug|eur-final|usa|eur|all|test [usa|eur]|soak [usa|eur]] [ninja args]" >&2
    exit 1
}

# the preset list a territory word (or `all`) stands for
presets_for()
{
    case "$1" in
        usa)  echo "debug final" ;;
        eur)  echo "eur-debug eur-final" ;;
        all|"") echo "debug final eur-debug eur-final" ;;
        debug|final|eur-debug|eur-final) echo "$1" ;;
        usa-debug) echo "debug" ;;      # the build-data.sh spelling, same tree
        usa-final) echo "final" ;;
        *) usage ;;
    esac
}

# The data a preset's exe compiles against and boots from.  CMake only gates
# on the generated headers (per territory); the BIGLUMP under the variant dir
# is what cd.cpp opens at run time, so check both here and say what to run.
check_data()
{
    preset="$1"
    case "$preset" in
        eur-*) terr=EUR ;;
        *)     terr=USA ;;
    esac
    ver=$(echo "${preset##*-}" | tr '[:lower:]' '[:upper:]')
    for f in "../out/$terr/include/BigLump.h" "../out/$terr/$ver/version/CD/BIGLUMP.BIN"; do
        if [ ! -f "$f" ]; then
            echo "missing $f - run: port/build-data.cmd $preset   (= $terr $ver)" >&2
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
    case "$preset" in
        eur-*) terr=EUR ;;
        *)     terr=USA ;;
    esac
    echo "=== soak ($preset): tier 1 + tier 2, all routes / all levels ==="
    python3 tests/run_tier.py --exe "build/$preset/sbsp.exe" --territory "$terr" \
        --selftest --tier1 --tier2 --logs "build/$preset/soak-logs"
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
    *)
        list=$(presets_for "$what") || exit 1
        for p in $list; do build_one "$p" "$@"; done
        ;;
esac

echo "PC build complete."
