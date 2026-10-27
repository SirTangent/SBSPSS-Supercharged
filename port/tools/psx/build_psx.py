#!/usr/bin/env python3
"""Build the original PlayStation executable, Spongey.cpe (issue #98):
port/build-psx.cmd runs this, with build-psx.sh's command line.

    build-psx [USA|EUR] [DEBUG|FINAL] [--identity-noline]      (defaults: USA DEBUG)

It configures and builds port/psx's CMake project (presets psx-<t>-<v>)
with Ninja: the 1999 PSY-Q toolchain in tools/psyq, driven as makefile.gaz
drove it, without MSYS2.  The territory's data must be built first
(port/build-data.cmd <t>): the generated headers in out/<T>/include are
compile inputs.  --identity-noline builds with PORT_LINE as 0, for
port/tools/psx_identity.py's line-insensitive DEBUG compare only.

One PS1 build at a time: the trees share sn.ini at the repository root and
info.mip under source/system, so a second build waits for the first
(a lock file in %TEMP%).  CMake and Ninja come from port/tools/buildenv.py.
"""
import msvcrt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))      # port/tools: buildenv, also under python -I
import buildenv                                 # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
LOCK = os.path.join(os.environ.get("TEMP", ROOT), "sbsp-psx.lock")


def usage():
    print("usage: port\\build-psx.cmd [USA|EUR] [DEBUG|FINAL] [--identity-noline]", file=sys.stderr)
    sys.exit(1)


def parse(argv):
    noline, words = False, []
    for a in argv:
        if a == "--identity-noline":
            noline = True
        elif a.startswith("-"):
            usage()                     # a mistyped option is never a territory (#89)
        else:
            words.append(a.upper())
    if len(words) > 2:
        usage()
    territory = words[0] if words else "USA"
    version = words[1] if len(words) > 1 else "DEBUG"
    if territory not in ("USA", "EUR") or version not in ("DEBUG", "FINAL"):
        usage()
    return territory, version, noline


def lock():
    f = open(LOCK, "a+")
    try:
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        print("Another PS1 build holds %s - waiting for it to finish..." % LOCK, file=sys.stderr)
        while True:
            try:
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                time.sleep(1)
    return f                            # held until the process exits


def main(argv):
    sys.stdout.reconfigure(line_buffering=True)
    if os.environ.get("SBSP_IDENTITY_NOLINE"):
        print("build-psx: SBSP_IDENTITY_NOLINE in the environment is ignored "
              "(psx_identity.py passes --identity-noline)", file=sys.stderr)
    territory, version, noline = parse(argv)
    if not os.path.isfile(os.path.join(ROOT, "out", territory, "include", "trans.h")):
        sys.exit("PSX BUILD FAILED: out/%s/include is missing - run: port\\build-data.cmd %s"
                 % (territory, territory.lower()))
    held = lock()
    cmake, ninja = buildenv.find_cmake(), buildenv.find_ninja()
    preset = "psx-%s-%s" % (territory.lower(), version.lower())
    src = os.path.join(ROOT, "port", "psx")
    configure = [cmake, "--preset", preset, "-DCMAKE_MAKE_PROGRAM=" + ninja.replace("\\", "/"),
                 "-DPython3_EXECUTABLE=" + sys.executable.replace("\\", "/"),
                 "-DSBSP_IDENTITY_NOLINE=" + ("ON" if noline else "OFF")]
    print("PS1 build: %s %s%s" % (territory, version, " (identity-noline)" if noline else ""))
    for cmd in (configure, [cmake, "--build", "--preset", preset]):
        if subprocess.run(cmd, cwd=src).returncode != 0:
            print("PSX BUILD FAILED", file=sys.stderr)
            return 1
    cpe = os.path.join(ROOT, "out", territory, version, "version", "CD", "Spongey.cpe")
    if not os.path.isfile(cpe):
        print("PSX BUILD FAILED: %s was not produced" % os.path.relpath(cpe, ROOT), file=sys.stderr)
        return 1
    print("PS1 build complete: %s" % os.path.relpath(cpe, ROOT).replace("\\", "/"))
    held.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
