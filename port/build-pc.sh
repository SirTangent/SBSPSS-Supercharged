#!/bin/bash
# Configure + build the PC (Win32) port: every preset by default.
#
#   port/build-pc.sh [<preset>|usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]|all] [extra ninja args...]
#   port/build-pc.sh test [usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]]  build, then ctest -L unit and -L playthrough on each tree
#                      (playthrough skipped, with a note, for a tree configured SBSP_PLAYTHROUGH=OFF)
#   port/build-pc.sh soak [usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]]  build, then the full Tier 1 + Tier 2 sweep on each tree
#   port/build-pc.sh parity64 [usa|eur] [debug|final]  build clang-cl x86 + x64, then the x64 A/B (streams, cross replay, cards)
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
# clangcl-eur-debug / clangcl-eur-final and clangcl-x64-eur-debug /
# clangcl-x64-eur-final are the EUR trees of both (issue #39): `clangcl eur`
# and `clangcl64 eur` name those pairs (`clangcl usa` = `clangcl`).
#
# SBSP_CODEVIEW=1 in the environment configures the MinGW trees with
# -DSBSP_CODEVIEW=ON (a .pdb beside every exe, for Visual Studio / WinDbg).
#
# SBSP_PLAYTHROUGH=OFF in the environment configures every tree with
# -DSBSP_PLAYTHROUGH=OFF (no Python needed, no playthrough tests registered):
#   SBSP_PLAYTHROUGH=OFF port/build-pc.sh test usa
# Unlike SBSP_CODEVIEW it is only passed when set, so the tree keeps what it
# was last configured with; SBSP_PLAYTHROUGH=ON turns the tests back on.
#
# Requires the MSYS2 mingw32 toolchain, and its Python for the playthrough
# tests (configure stops without it; SBSP_PLAYTHROUGH=OFF opts out):
#   pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja mingw-w64-i686-python

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
    echo "usage: port/build-pc.sh [debug|final|usa-debug|usa-final|eur-debug|eur-final|clangcl-debug|clangcl-final|clangcl-eur-debug|clangcl-eur-final|clangcl-x64-debug|clangcl-x64-final|clangcl-x64-eur-debug|clangcl-x64-eur-final|usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]|all|test [usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]]|soak [usa|eur|clangcl [usa|eur]|clangcl64 [usa|eur]]|parity64 [usa|eur] [debug|final]] [ninja args]" >&2
    exit 1
}

lower()
{
    echo "$1" | tr '[:upper:]' '[:lower:]'          # any case, like build-data.sh
}

# the preset list a word (or `all`) stands for; the territory word that may
# follow `clangcl` / `clangcl64` is the second argument (USA when empty)
presets_for()
{
    w=$(lower "$1")
    t=$(lower "${2:-usa}")
    case "$t" in
        usa) e= ;;
        eur) e=eur- ;;
        *)   usage ;;
    esac
    case "$w" in
        usa)  echo "debug final" ;;
        eur)  echo "eur-debug eur-final" ;;
        clangcl) echo "clangcl-${e}debug clangcl-${e}final" ;;
        clangcl64) echo "clangcl-x64-${e}debug clangcl-x64-${e}final" ;;
        all|"") echo "debug final eur-debug eur-final" ;;
        debug|final|eur-debug|eur-final|clangcl-debug|clangcl-final|clangcl-eur-debug|clangcl-eur-final) echo "$w" ;;
        clangcl-x64-debug|clangcl-x64-final|clangcl-x64-eur-debug|clangcl-x64-eur-final) echo "$w" ;;
        usa-debug) echo "debug" ;;      # the build-data.sh spelling, same tree
        usa-final) echo "final" ;;
        *) usage ;;
    esac
}

# the territory a preset builds, for its data and the tier runner's --territory
territory_of()
{
    case "$1" in
        eur-*|*-eur-*) echo EUR ;;      # eur-debug, clangcl-eur-final, clangcl-x64-eur-debug, ...
        *)             echo USA ;;
    esac
}

# The data a preset's exe compiles against and boots from.  CMake only gates
# on the generated headers; out/<T>/cd is what cd.cpp opens at run time (the
# same files for DEBUG and FINAL), so check both here and say what to run.
check_data()
{
    preset="$1"
    terr=$(territory_of "$preset")
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
    # extra args go to the build, so the configure-time switches come from the
    # environment; SBSP_PLAYTHROUGH only when set (see the header)
    case "$preset" in
        clangcl-*) cmake --preset "$preset" ${SBSP_PLAYTHROUGH:+"-DSBSP_PLAYTHROUGH=$SBSP_PLAYTHROUGH"} ;;
        *)         cmake --preset "$preset" -DSBSP_CODEVIEW="${SBSP_CODEVIEW:-0}" \
                       ${SBSP_PLAYTHROUGH:+"-DSBSP_PLAYTHROUGH=$SBSP_PLAYTHROUGH"} ;;
    esac
    cmake --build --preset "$preset" "$@"
}

test_one()
{
    preset="$1"
    echo "=== ctest ($preset): unit ==="
    # SBSP_TEST_STRICT: this tree owns its data (build-data staged the movies;
    # the fixtures and the music are committed), so a test that finds an input
    # missing and would otherwise exit 77 (ctest: skipped) fails instead -
    # a skip here is a lost input, not a choice (tests/test_skip.h, issue #61)
    SBSP_TEST_STRICT=1 ctest --test-dir "build/$preset" --output-on-failure -L unit --no-tests=error
    # --no-tests=error turns "the label matched nothing" into a failure, which
    # is the point: a playthrough label that quietly registered zero tests used
    # to read green.  A shim-only tree (-DSBSP_BUILD_GAME=OFF) legitimately has
    # no playthrough tests, though, so say so and skip rather than fail - the
    # strict form is what CI runs, and CI always builds the game.
    if [ ! -f "build/$preset/sbsp.exe" ]; then
        echo "=== ctest ($preset): playthrough SKIPPED - no sbsp.exe (SBSP_BUILD_GAME=OFF?) ==="
        return
    fi
    # Likewise a tree configured with -DSBSP_PLAYTHROUGH=OFF (CMakeLists.txt,
    # the no-Python opt-out; SBSP_PLAYTHROUGH=OFF in the environment, or a
    # hand configure) registers none on purpose.  The cache keeps the value
    # across build_one's re-configure, so read it from there; any of CMake's
    # false spellings counts.
    pt=$(sed -n 's/^SBSP_PLAYTHROUGH:BOOL=//p' "build/$preset/CMakeCache.txt" 2>/dev/null | tr -d '\r' | tr '[:lower:]' '[:upper:]')
    case "$pt" in
        0|OFF|NO|FALSE|N|IGNORE|NOTFOUND|*-NOTFOUND)
            echo "=== ctest ($preset): playthrough SKIPPED - opted out with SBSP_PLAYTHROUGH=OFF (SBSP_PLAYTHROUGH=ON to re-enable) ==="
            return ;;
    esac
    echo "=== ctest ($preset): playthrough ==="
    SBSP_TEST_STRICT=1 ctest --test-dir "build/$preset" --output-on-failure -L playthrough --no-tests=error
}

soak_one()
{
    preset="$1"
    terr=$(territory_of "$preset")
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
#
# The territory picks the trees (clangcl-<v> / clangcl-eur-<v> and their x64
# pair) and the runner's --territory, which adds the EUR-only routes
# (play_trailer); a USA A/B keeps its old directory, build/parity64-<v>.
parity64()
{
    e="$1"                      # the preset infix: "" (USA) or "eur-"
    v="$2"
    terr=$(territory_of "clangcl-$e$v")
    d="build/parity64-$e$v"
    rc=0
    rm -rf "$d"
    echo "=== parity64 ($terr $v): x86 baseline ==="
    # --no-replay: the baseline's own determinism replay is already a ctest,
    # and the recording leg 3 needs is written by the first pass regardless.
    python3 tests/run_tier.py --exe "build/clangcl-$e$v/sbsp.exe" --territory "$terr" --tier1 --tier2 \
        --no-replay --logs "$d/L32" --keep-artifacts "$d/A32" || rc=1
    echo "=== parity64 ($terr $v): x64, same tiers ==="
    python3 tests/run_tier.py --exe "build/clangcl-x64-$e$v/sbsp.exe" --territory "$terr" --tier1 --tier2 \
        --logs "$d/L64" --compare-frames "$d/L32" || rc=1
    echo "=== parity64 ($terr $v): x64 replays the x86 recordings ==="
    python3 tests/run_tier.py --exe "build/clangcl-x64-$e$v/sbsp.exe" --territory "$terr" --tier1 \
        --logs "$d/L64x" --compare-frames "$d/L32" --replay-from "$d/A32" || rc=1
    return $rc
}

# presets_for runs in a command substitution, so its usage exit must be
# re-checked here or a bad word would silently build nothing.
case "$what" in
    test)
        list=$(presets_for "${1:-all}" "${2:-}") || exit 1
        for p in $list; do build_one "$p"; done
        for p in $list; do test_one "$p"; done
        ;;
    soak)
        list=$(presets_for "${1:-all}" "${2:-}") || exit 1
        for p in $list; do build_one "$p"; done
        for p in $list; do soak_one "$p"; done
        ;;
    parity64)
        # [usa|eur] [debug|final]: USA and DEBUG by default, the variant the
        # A/B is green on.  USA FINAL has a known 39-frame divergence
        # (campaign 6858-6897, an uninitialised read - conv_pc.md, issue
        # #39), so `parity64 final` is expected to report a failure; EUR
        # FINAL does not show it.
        [ $# -le 2 ] || usage
        t=usa
        v=debug
        for w in "$@"; do
            case "$(lower "$w")" in
                usa|eur)     t=$(lower "$w") ;;
                debug|final) v=$(lower "$w") ;;
                *)           usage ;;
            esac
        done
        case "$t" in
            eur) e=eur- ;;
            *)   e= ;;
        esac
        build_one "clangcl-$e$v"
        build_one "clangcl-x64-$e$v"
        parity64 "$e" "$v"
        ;;
    *)
        # `clangcl` / `clangcl64` may be followed by a territory word; what
        # comes after that is ninja's
        terr=
        case "$(lower "$what")" in
            clangcl|clangcl64)
                case "$(lower "${1:-}")" in
                    usa|eur) terr="$1"; shift ;;
                esac ;;
        esac
        list=$(presets_for "$what" "$terr") || exit 1
        for p in $list; do build_one "$p" "$@"; done
        ;;
esac

echo "PC build complete."
