# Sourced by port/build-data.sh and port/build-psx.sh (issue #51), from the
# repo root:
#   require_msys2 <script> "<tools>" "$@"
# stops unless this is MSYS2's own bash, started as a login shell, with each
# of <tools> in its /usr/bin.  Git Bash sets MSYSTEM too, and could carry a
# make or a flock, but it has no pacman, and its /tmp is not MSYS2's (ccpsx's
# fixed C:/msys64/tmp/PQ3 and the PS1 build lock live in MSYS2's).
require_msys2()
{
    local script=$1 tools=$2 lacks=() t
    shift 2
    [ -n "$MSYSTEM" ] || lacks+=("MSYSTEM (not a login shell)")
    [ -x /usr/bin/pacman ] || lacks+=("/usr/bin/pacman (not MSYS2)")
    for t in $tools; do
        [ -x "/usr/bin/$t" ] || lacks+=("/usr/bin/$t")
    done
    [ ${#lacks[@]} -eq 0 ] && return 0
    echo "port/$script.sh runs only in MSYS2's bash, as a login shell, with ${tools// / and } installed" >&2
    echo "(this shell lacks:$(printf ' %s;' "${lacks[@]}"))  Run:" >&2
    echo "  port\\$script.cmd $*        (cmd, PowerShell; port/$script.cmd from Git Bash)" >&2
    echo "  MSYSTEM=MSYS C:/msys64/usr/bin/bash -lc 'cd \"$PWD\" && port/$script.sh $*'" >&2
    exit 1
}
