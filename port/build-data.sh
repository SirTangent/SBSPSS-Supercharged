#!/bin/bash
# Build the SBSPSS game data (BigLump.Bin + generated headers) on modern Windows.
#
# Runs the original makefile.gfx with the MSYS2 runtime (make/sh/coreutils/perl)
# driving the converter EXEs: the ones rebuilt from source as x64 exes in
# port/tools/x64 (issue #48: MkData, Tga2Gfx, scripter, lznp so far), the
# repo's original Win32 ones for the rest.  --vintage-tools runs the
# originals for every converter, for an A/B of the outputs (lznp's original
# is 16-bit, so that one is the 32-bit port/tools/lznp.exe).
#
# build/globals.mak gives MSYS2's make the MSYS2 utilities and PATH in place
# of the 1999 cygwin ones, which crash on Windows 10/11 (issue #16), so the
# make line below is the same one a bare make from an MSYS2 shell would
# take.  MSYS2's bash only, as for build-psx.sh (issue #51): Git Bash finds a
# native Windows make, whose `sh -c` cuts the ~9K sprite-sheet command at
# ~8K, so parkgrab writes nothing (issue #42).  make is run from MSYS2's
# /usr/bin by absolute path.
#
# Usage (from an MSYS2 shell, or port\build-data.cmd from cmd, PowerShell or
# Git Bash):
#   port/build-data.sh [usa|eur]                  # default usa
#   port/build-data.sh <preset>                   # debug|final|usa-*|eur-*: the build-pc.sh
#                                                 # words; the variant half is ignored (note below)
#   port/build-data.sh USA|EUR DEBUG|FINAL        # also choose the PSX tree the vintage
#                                                 # make writes into (out/<T>/<V>/version/CD)
#   ... --vintage-tools                           # with any of the above: the 1999 converters
#
# The x64 converters are LFS objects; an unmaterialised pointer stops the
# build rather than falling back to the 1999 exes.  BIGLUMP.BIN from the
# rebuilt MkData differs from the vintage one in one byte, a bank size the
# 1999 MkData left uninitialised (port/docs/conv_pc.md, "x64 data tools").
#
# The PC data is per TERRITORY only (issue #35): makefile.gfx has no VERSION
# conditional, so a DEBUG and a FINAL data build are byte-identical.  The CD
# files the PC exe reads are staged once, into out/<T>/cd/, and both variants'
# exes boot from there (port/psyq/cd/cd.cpp resolveDataRoot).  VERSION only
# selects which PSX build tree the vintage make writes its own outputs into.
set -e

cd "$(dirname "$0")/.."

. port/require-msys2.sh
require_msys2 build-data "make" "$@"

usage()
{
    echo "usage: port/build-data.sh [usa|eur]  |  port/build-data.sh [usa-|eur-]debug|final  |  port/build-data.sh USA|EUR DEBUG|FINAL" >&2
    echo "       [--vintage-tools]  (any form: run the 1999 converters, not port/tools/x64)" >&2
    exit 1
}
# The tool set is an argument and goes to make on its command line, which
# outranks the environment: an SBSP_VINTAGE_TOOLS left in a shell profile
# cannot change it.
VINTAGE=0
POS=()
for a in "$@"; do
    case "$a" in
        --vintage-tools) VINTAGE=1 ;;
        -*) usage ;;                # a mistyped option is never a territory
        *) POS+=("$a") ;;
    esac
done
[ ${#POS[@]} -le 2 ] || usage
set -- "${POS[@]}"
VERSION=DEBUG
case "$(echo "${1:-USA}" | tr '[:upper:]' '[:lower:]')" in
    debug|final|usa-debug|usa-final)
                     TERRITORY=USA
                     echo "note: '$1' -> USA (PC data is per territory; the variant word is ignored)" ;;
    eur-debug|eur-final)
                     TERRITORY=EUR
                     echo "note: '$1' -> EUR (PC data is per territory; the variant word is ignored)" ;;
    usa|eur|jap)     TERRITORY=$(echo "$1" | tr '[:lower:]' '[:upper:]')
                     VERSION=$(echo "${2:-DEBUG}" | tr '[:lower:]' '[:upper:]') ;;
    *) usage ;;
esac
case "$VERSION" in DEBUG|FINAL) ;; *) usage ;; esac
echo "Data build: TERRITORY=$TERRITORY (vintage make VERSION=$VERSION) -> out/$TERRITORY"

if [ "$VINTAGE" = 1 ]; then
    echo "Converters: the 1999 Win32 exes (--vintage-tools)"
else
    for tool in MkData Tga2Gfx scripter lznp; do
        exe="port/tools/x64/$tool.exe"
        if [ ! -f "$exe" ] || [ "$(stat -c%s "$exe")" -lt 4096 ]; then
            echo "ERROR: $exe is missing or an unmaterialised Git-LFS pointer." >&2
            echo "       Run: git lfs pull --include \"port/tools/x64/*.exe\"" >&2
            echo "       (or add --vintage-tools to build with the 1999 converters)" >&2
            exit 1
        fi
    done
    echo "Converters: port/tools/x64 (MkData, Tga2Gfx, scripter, lznp), the 1999 Win32 exes for the rest"
fi

# Until issue #15 three makefile.gfx recipes redirected to `nul`, which
# MSYS2's sh creates as a real file that Explorer and del cannot remove.
# Trees built before the fix still have one at the root; MSYS2's rm can.
rm -f nul

# MkActor.exe compresses every pack through system("lznp ..."), and the MSVC
# CRT's system() locates cmd.exe via COMSPEC.  An MSYS2 shell started without
# a console (CI, an agent's subprocess) can lack it, and the only symptom is a
# bare "Could not open temp Pak file Actor.Pak" from every actor.
export COMSPEC="${COMSPEC:-$(cygpath -w "${SYSTEMROOT:-C:/WINDOWS}/system32/cmd.exe")}"
/usr/bin/make -r -f makefile.gfx VERSION="$VERSION" TERRITORY="$TERRITORY" USER_NAME=CDBUILD \
    SBSP_VINTAGE_TOOLS="$VINTAGE"

# Stage the CD files the PC exe reads into the per-territory directory (M6
# IXA, M7 movies, M8 #35 BIGLUMP).  BIGLUMP comes from the vintage make's
# PSX tree above; on PSX the makefile.gaz cddata rule copies the rest into
# the CD image.  Guard against an unmaterialised Git-LFS pointer file (a few
# hundred bytes, not ~127MB) and truncated movies.
CD_DIR="out/$TERRITORY/cd"
mkdir -p "$CD_DIR"

stage()
{
    src="$1"; dst="$2"
    if [ ! -f "$src" ] || [ "$(stat -c%s "$src")" -lt 1048576 ]; then
        echo "ERROR: $src is missing, truncated, or an unmaterialised Git-LFS pointer." >&2
        echo "       (for data/CDData/*.Ixa run: git lfs pull)" >&2
        exit 1
    fi
    if [ ! -f "$dst" ] || [ "$src" -nt "$dst" ]; then
        cp "$src" "$dst"
        echo "Staged $dst"
    fi
}

stage "out/$TERRITORY/$VERSION/version/CD/BIGLUMP.BIN" "$CD_DIR/BIGLUMP.BIN"
stage "data/CDData/Track1.Ixa" "$CD_DIR/TRACK1.IXA"
for movie in thq climax intro demo; do
    stage "data/CDData/$movie.str" "$CD_DIR/$(echo "$movie" | tr a-z A-Z).STR"
done

echo "Data build complete: out/$TERRITORY (PC data: $CD_DIR)"
