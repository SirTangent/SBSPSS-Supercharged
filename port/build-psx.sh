#!/bin/bash
# Build the original PlayStation executable (makefile.gaz) on modern Windows.
#
# Same harness as build-data.sh: the MSYS2 runtime (make/sh/coreutils) drives
# the vintage PSY-Q compilers, which still run on Win11 even though the 1999
# cygwin utilities do not (build/globals.mak gives MSYS2's make its tools and
# PATH). PSYQ_PATH goes on the make command line because globals.mak exports
# it empty; ccpsx looks for sn.ini in the cwd (its paths are
# repo-root-relative), so stage a copy if missing.
#
# Usage: port/build-psx.sh [USA|EUR] [DEBUG|FINAL] [--identity-noline]
#        defaults: USA DEBUG; --identity-noline is comparison-only (below)
#        port\build-psx.cmd [...]                     # from cmd or PowerShell
#
# Constraints (issue #51), all enforced below:
# - MSYS2's bash only.  Git Bash finds a native Windows make, which cannot
#   read the POSIX PATH (it dies on glecho), and its /tmp is not MSYS2's.
#   make and flock are run from MSYS2's /usr/bin by absolute path.
# - USA|EUR and DEBUG|FINAL, upper-cased here: `debug` would compile with
#   -D__VERSION_debug__ and NTFS would file the result under out/USA/DEBUG.
# - One PS1 build on the machine at a time: ccpsx writes its intermediate to
#   the fixed C:/msys64/tmp/PQ3.  make runs under flock /tmp/sbsp-psx.lock,
#   so a second build waits for the first.
# - Every build overwrites Spongey.cpe; copy it aside before comparing two
#   (port/tools/psx_identity.py build does).
set -e

cd "$(dirname "$0")/.."

if [ -z "$MSYSTEM" ] || [ ! -x /usr/bin/make ] || [ ! -x /usr/bin/flock ]; then
    echo "port/build-psx.sh runs only in MSYS2's bash, as a login shell, with make installed" >&2
    echo "(this shell: MSYSTEM=${MSYSTEM:-unset}, /usr/bin/make $([ -x /usr/bin/make ] && echo found || echo missing)).  Run:" >&2
    echo "  port\\build-psx.cmd $*        (from cmd or PowerShell)" >&2
    echo "  MSYSTEM=MSYS C:/msys64/usr/bin/bash -lc 'cd \"$PWD\" && port/build-psx.sh $*'" >&2
    exit 1
fi

usage()
{
    echo "usage: port/build-psx.sh [USA|EUR] [DEBUG|FINAL] [--identity-noline]" >&2
    exit 1
}

# Comparison-only, for port/tools/psx_identity.py: --identity-noline builds
# with PORT_LINE (source/system/dbg.h) as 0, so two DEBUG builds compare
# without counting line shifts.  An argument, never the environment, so a
# variable left in a shell profile cannot turn it on for a normal build.
NOLINE=()
POS=()
for a in "$@"; do
    case "$a" in
        --identity-noline) NOLINE=(SBSP_IDENTITY_NOLINE=1) ;;
        *) POS+=("$a") ;;
    esac
done
if [ -n "${SBSP_IDENTITY_NOLINE:-}" ]; then
    echo "build-psx.sh: SBSP_IDENTITY_NOLINE in the environment is ignored" \
         "(psx_identity.py passes --identity-noline)" >&2
fi

[ ${#POS[@]} -le 2 ] || usage
TERRITORY=$(echo "${POS[0]:-USA}" | tr '[:lower:]' '[:upper:]')
VERSION=$(echo "${POS[1]:-DEBUG}" | tr '[:lower:]' '[:upper:]')
case "$TERRITORY" in USA|EUR) ;; *) usage ;; esac
case "$VERSION" in DEBUG|FINAL) ;; *) usage ;; esac

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

# make tracks no -D flags, so objects from the other mode would be linked
# as up to date.  The tree records its mode (no record = normal, as every
# tree before #81 was) and a switch starts it clean.
MODE=normal
[ ${#NOLINE[@]} -eq 0 ] || MODE=noline
STAMP="out/$TERRITORY/$VERSION/identity-mode"
LAST=$(cat "$STAMP" 2>/dev/null || echo normal)
if [ "$LAST" != "$MODE" ]; then
    echo "build-psx.sh: the tree was last built $LAST, this build is $MODE - starting clean"
    rm -rf "out/$TERRITORY/$VERSION/CD/objs" "out/$TERRITORY/$VERSION/deps" \
           "out/$TERRITORY/$VERSION/version/CD/Spongey.cpe"
fi
echo "$MODE" > "$STAMP"

LOCK=/tmp/sbsp-psx.lock
/usr/bin/flock -n "$LOCK" true || echo "Another PS1 build holds $LOCK - waiting for it to finish..." >&2

# Targets 'dirs link' only: the full 'all' also runs 'cddata' (CPE->BIN,
# buildcd, ISO), which needs 16-bit tools (cpe2bin) and only matters for
# pressing a physical disc - the port consumes the linked output directly.
if ! /usr/bin/flock "$LOCK" /usr/bin/make -r -f makefile.gaz \
    VERSION="$VERSION" TERRITORY="$TERRITORY" USER_NAME=CDBUILD \
    "PSYQ_PATH=$PWD/tools/psyq/bin/egcs" \
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
