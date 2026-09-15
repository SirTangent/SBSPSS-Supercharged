#!/bin/bash
# Build the SBSPSS game data (BigLump.Bin + generated headers) on modern Windows.
#
# Runs the original makefile.gfx with the MSYS2 runtime (make/sh/coreutils/perl)
# driving the repo's original Win32 converter EXEs. The PATH assignment on the
# make command line overrides the vintage-cygwin PATH that build/globals.mak:141
# force-exports (those 1999 cygwin binaries crash on Windows 10/11).
#
# Usage (from an MSYS2 shell, or: C:\msys64\usr\bin\bash.exe -l <this script>):
#   port/build-data.sh [usa|eur]                  # default usa
#   port/build-data.sh <preset>                   # debug|final|usa-*|eur-*: the build-pc.sh
#                                                 # words; the variant half is ignored (note below)
#   port/build-data.sh USA|EUR DEBUG|FINAL        # also choose the PSX tree the vintage
#                                                 # make writes into (out/<T>/<V>/version/CD)
#
# The PC data is per TERRITORY only (issue #35): makefile.gfx has no VERSION
# conditional, so a DEBUG and a FINAL data build are byte-identical.  The CD
# files the PC exe reads are staged once, into out/<T>/cd/, and both variants'
# exes boot from there (port/psyq/cd/cd.cpp resolveDataRoot).  VERSION only
# selects which PSX build tree the vintage make writes its own outputs into.
set -e

cd "$(dirname "$0")/.."

usage()
{
    echo "usage: port/build-data.sh [usa|eur]  |  port/build-data.sh [usa-|eur-]debug|final  |  port/build-data.sh USA|EUR DEBUG|FINAL" >&2
    exit 1
}
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

# port/tools first: its modern lznp.exe must shadow the 16-bit tools/lznp.exe.
# PATH and Path both overridden (globals.mak exports both spellings), and every
# tool variable globals.mak pins to the vintage tools/cygwin binaries is
# redirected to the MSYS2 equivalents - the 1999 cygwin ones crash on Win11.
BUILD_PATH="/usr/bin:$PWD/port/tools:$PWD/tools:$PWD/tools/Data/bin:$PWD/tools/psyq/bin"
# MkActor.exe compresses every pack through system("lznp ..."), and the MSVC
# CRT's system() locates cmd.exe via COMSPEC.  An MSYS2 shell started without
# a console (CI, an agent's subprocess) can lack it, and the only symptom is a
# bare "Could not open temp Pak file Actor.Pak" from every actor.
export COMSPEC="${COMSPEC:-$(cygpath -w "${SYSTEMROOT:-C:/WINDOWS}/system32/cmd.exe")}"
make -r -f makefile.gfx \
    VERSION="$VERSION" TERRITORY="$TERRITORY" USER_NAME=CDBUILD \
    "PATH=$BUILD_PATH" "Path=$BUILD_PATH" \
    MKDIR=mkdir ECHO=echo MV=mv DATE=date SED=sed \
    RMDIR=rmdir LS=ls "ATTRIB=chmod +w"

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
