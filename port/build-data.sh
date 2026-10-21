#!/bin/bash
# Build the SBSPSS game data (BigLump.Bin + generated headers) on modern Windows.
#
# The data build is the CMake + Ninja project in port/data (issue #95),
# driven by port/tools/data/build_data.py, which port/build-data.cmd runs.
# This script hands its arguments to build-data.cmd, so a bash shell (MSYS2's
# or Git Bash) and cmd run the same thing.  It needs Python 3 on the Windows
# PATH, and CMake + Ninja (MSYS2's mingw32 ones, or any on the PATH).
#
# Usage:
#   port/build-data.sh [usa|eur]                  # default usa
#   port/build-data.sh <preset>                   # debug|final|usa-*|eur-*: the build-pc.sh
#                                                 # words; the variant half is ignored
#   port/build-data.sh USA|EUR DEBUG|FINAL        # the version word is accepted and ignored
#   ... --vintage-tools                           # with any of the above: the 1999 converters
#
# The PC data is per TERRITORY only (issue #35): the data never depended on
# the variant, so one build serves the DEBUG and FINAL exes, which boot from
# out/<T>/cd (port/psyq/cd/cd.cpp resolveDataRoot).  The x64 converters are
# Git LFS objects; an unmaterialised pointer stops the build rather than
# falling back to the 1999 exes (port/docs/conv_pc.md, "x64 data tools").
set -e
cd "$(dirname "$0")/.."
exec cmd.exe //c "$(cygpath -w port/build-data.cmd)" "$@"
