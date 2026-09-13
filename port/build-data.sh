#!/bin/bash
# Build the SBSPSS game data (BigLump.Bin + generated headers) on modern Windows.
#
# Runs the original makefile.gfx with the MSYS2 runtime (make/sh/coreutils/perl)
# driving the repo's original Win32 converter EXEs. The PATH assignment on the
# make command line overrides the vintage-cygwin PATH that build/globals.mak:141
# force-exports (those 1999 cygwin binaries crash on Windows 10/11).
#
# Usage (from an MSYS2 shell, or: C:\msys64\usr\bin\bash.exe -l <this script>):
#   port/build-data.sh [TERRITORY] [VERSION]      # defaults: USA DEBUG
#   port/build-data.sh <preset>                   # debug|final|usa-*|eur-* (as build-pc.sh)
set -e

cd "$(dirname "$0")/.."

#   port/build-data.sh [TERRITORY] [VERSION]   e.g. USA DEBUG, EUR FINAL (any case)
#   port/build-data.sh <preset>                 the build-pc.sh spelling:
#                                               debug final usa-debug usa-final eur-debug eur-final
usage()
{
    echo "usage: port/build-data.sh [USA|EUR] [DEBUG|FINAL]  |  port/build-data.sh [usa-|eur-]debug|final" >&2
    exit 1
}
case "$(echo "${1:-USA}" | tr '[:upper:]' '[:lower:]')" in
    debug|usa-debug) TERRITORY=USA; VERSION=DEBUG ;;
    final|usa-final) TERRITORY=USA; VERSION=FINAL ;;
    eur-debug)       TERRITORY=EUR; VERSION=DEBUG ;;
    eur-final)       TERRITORY=EUR; VERSION=FINAL ;;
    usa|eur|jap)     TERRITORY=$(echo "$1" | tr '[:lower:]' '[:upper:]')
                     VERSION=$(echo "${2:-DEBUG}" | tr '[:lower:]' '[:upper:]') ;;
    *) usage ;;
esac
case "$VERSION" in DEBUG|FINAL) ;; *) usage ;; esac
echo "Data build: TERRITORY=$TERRITORY VERSION=$VERSION -> out/$TERRITORY"

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

# Stage the XA speech stream next to BIGLUMP.BIN (M6) - on PSX this is
# makefile.gaz's cddata rule copying it into the CD image. Guard against an
# unmaterialised Git-LFS pointer file (a few hundred bytes, not ~127MB).
IXA_SRC="data/CDData/Track1.Ixa"
IXA_DST="out/$TERRITORY/$VERSION/version/CD/TRACK1.IXA"
if [ ! -f "$IXA_SRC" ] || [ "$(stat -c%s "$IXA_SRC")" -lt 1048576 ]; then
    echo "ERROR: $IXA_SRC is missing or is an unmaterialised Git-LFS pointer." >&2
    echo "       Run: git lfs pull" >&2
    exit 1
fi
if [ ! -f "$IXA_DST" ] || [ "$IXA_SRC" -nt "$IXA_DST" ]; then
    cp "$IXA_SRC" "$IXA_DST"
    echo "Staged $IXA_DST"
fi

# Stage the FMV movies (M7) - raw-XA .STR files, same layout as the IXA.
# Not LFS-tracked, but keep the same size sanity guard for uniformity.
for movie in thq climax intro demo; do
    STR_SRC="data/CDData/$movie.str"
    STR_DST="out/$TERRITORY/$VERSION/version/CD/$(echo "$movie" | tr a-z A-Z).STR"
    if [ ! -f "$STR_SRC" ] || [ "$(stat -c%s "$STR_SRC")" -lt 1048576 ]; then
        echo "ERROR: $STR_SRC is missing or truncated." >&2
        exit 1
    fi
    if [ ! -f "$STR_DST" ] || [ "$STR_SRC" -nt "$STR_DST" ]; then
        cp "$STR_SRC" "$STR_DST"
        echo "Staged $STR_DST"
    fi
done

echo "Data build complete: out/$TERRITORY"
