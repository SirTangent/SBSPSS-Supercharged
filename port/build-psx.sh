#!/bin/bash
# Build the original PlayStation executable (makefile.gaz) on modern Windows.
#
# Same harness as build-data.sh: the MSYS2 runtime (make/sh/coreutils) drives
# the vintage PSY-Q compilers, which still run on Win11 even though the 1999
# cygwin utilities do not. PSYQ_PATH goes on the make command line because
# globals.mak exports it empty; ccpsx looks for sn.ini in the cwd (its paths
# are repo-root-relative), so stage a copy if missing.
#
# Usage: port/build-psx.sh [TERRITORY] [VERSION]      # defaults: USA DEBUG
set -e

cd "$(dirname "$0")/.."

TERRITORY="${1:-USA}"
VERSION="${2:-DEBUG}"

# Always refresh: the toolchain copy is authoritative.
cp -f tools/psyq/bin/egcs/sn.ini sn.ini

# The PSY-Q tools take DOS-style /flags; stop MSYS2 from rewriting them as paths.
export MSYS2_ARG_CONV_EXCL='*'

# GNU Make remakes included makefiles (the .d autodependency files, via
# `include $(ALL_CODE_DEPS)`) before running any explicit goal - including
# 'dirs' below. That first dep-file compile writes its scratch output
# through $(TEMP_DIR), which only the 'dirs' target creates, so on a truly
# fresh out/ tree (no prior local build ever populated it) the very first
# .d rule fails before 'dirs' gets a chance to run. Every established dev
# checkout has this directory already, which is why it's gone unnoticed.
mkdir -p "out/$TERRITORY/$VERSION/CD/temp"

BUILD_PATH="/usr/bin:$PWD/port/tools:$PWD/tools:$PWD/tools/psyq/bin:$PWD/tools/psyq/bin/egcs"

# Comparison-only, for port/tools/psx_identity.py: SBSP_IDENTITY_NOLINE=1
# builds with PORT_LINE (source/system/dbg.h) as 0, so two DEBUG builds
# compare without counting line shifts.  Never set it for a build anyone runs.
NOLINE=()
if [ "${SBSP_IDENTITY_NOLINE:-}" = 1 ]; then
    NOLINE=(SBSP_IDENTITY_NOLINE=1)
fi

# Targets 'dirs link' only: the full 'all' also runs 'cddata' (CPE->BIN,
# buildcd, ISO), which needs 16-bit tools (cpe2bin) and only matters for
# pressing a physical disc - the port consumes the linked output directly.
if ! make -r -f makefile.gaz \
    VERSION="$VERSION" TERRITORY="$TERRITORY" USER_NAME=CDBUILD \
    "PATH=$BUILD_PATH" "Path=$BUILD_PATH" \
    "PSYQ_PATH=$PWD/tools/psyq/bin/egcs" \
    MKDIR=mkdir ECHO=echo MV=mv DATE=date SED=sed \
    RMDIR=rmdir LS=ls "ATTRIB=chmod +w" \
    "${NOLINE[@]}" \
    dirs link
then
    echo "PSX BUILD FAILED" >&2
    exit 1
fi

# Never report success without the artifact: a login shell can swallow the
# make exit status, which once let a broken regression build look clean.
CPE="out/$TERRITORY/$VERSION/version/CD/Spongey.cpe"
if [ ! -f "$CPE" ]; then
    echo "PSX BUILD FAILED: $CPE was not produced" >&2
    exit 1
fi

echo "PSX build complete: $CPE"
