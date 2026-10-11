#!/bin/bash
# Build the SBSPSS game data (BigLump.Bin + generated headers) on modern Windows.
#
# Runs the original makefile.gfx with the MSYS2 runtime (make/sh/coreutils/perl)
# driving the repo's original Win32 converter EXEs.  build/globals.mak gives
# MSYS2's make the MSYS2 utilities and PATH in place of the 1999 cygwin ones,
# which crash on Windows 10/11 (issue #16), so the make line below is the
# same one a bare make from an MSYS2 shell would take.  MSYS2's bash only, as
# for build-psx.sh (issue #51): Git Bash finds a native Windows make.
#
# Usage (from an MSYS2 shell, or port\build-data.cmd from cmd / PowerShell):
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

if [ -z "$MSYSTEM" ] || [ ! -x /usr/bin/make ]; then
    echo "port/build-data.sh runs only in MSYS2's bash, as a login shell, with make installed" >&2
    echo "(this shell: MSYSTEM=${MSYSTEM:-unset}, /usr/bin/make $([ -x /usr/bin/make ] && echo found || echo missing)).  Run:" >&2
    echo "  port\\build-data.cmd $*        (from cmd or PowerShell)" >&2
    echo "  MSYSTEM=MSYS C:/msys64/usr/bin/bash -lc 'cd \"$PWD\" && port/build-data.sh $*'" >&2
    exit 1
fi

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

# Until issue #15 three makefile.gfx recipes redirected to `nul`, which
# MSYS2's sh creates as a real file that Explorer and del cannot remove.
# Trees built before the fix still have one at the root; MSYS2's rm can.
rm -f nul

# MkActor.exe compresses every pack through system("lznp ..."), and the MSVC
# CRT's system() locates cmd.exe via COMSPEC.  An MSYS2 shell started without
# a console (CI, an agent's subprocess) can lack it, and the only symptom is a
# bare "Could not open temp Pak file Actor.Pak" from every actor.
export COMSPEC="${COMSPEC:-$(cygpath -w "${SYSTEMROOT:-C:/WINDOWS}/system32/cmd.exe")}"
/usr/bin/make -r -f makefile.gfx VERSION="$VERSION" TERRITORY="$TERRITORY" USER_NAME=CDBUILD

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
