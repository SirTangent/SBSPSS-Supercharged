#!/usr/bin/env python3
"""Build the game data, out/<T>/ (issue #95): port/build-data.cmd runs this.

It configures and builds the CMake data project, port/data (presets
data-usa / data-eur), with Ninja.  The command line is build-data's as it
was under make, so callers don't change:

    build-data [usa|eur]              default usa
    build-data <preset>               debug|final|usa-*|eur-*, as build-pc; the variant is ignored
    build-data USA|EUR DEBUG|FINAL    the version is ignored too
    ... --vintage-tools               any form: the 1999 converters, not port/tools/x64

The data is per territory only: makefile.gfx never depended on the
variant, and a DEBUG and a FINAL exe read the same out/<T>/cd.  Each
build passes the converter set and clears the test-only final.dat
override, so neither carries over from an earlier configure.

CMake and Ninja come from port/tools/buildenv.py, as for the PC build: the
PATH, else Visual Studio's "C++ CMake tools for Windows" (issue #98).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))      # port/tools: buildenv, also under python -I
import buildenv                                 # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TOOLS = ["MkData", "Tga2Gfx", "scripter", "lznp", "MkActor", "MkLevel", "parkgrab", "transtext"]


def usage():
    print("usage: build-data [usa|eur] | [usa-|eur-]debug|final | USA|EUR DEBUG|FINAL  [--vintage-tools]",
          file=sys.stderr)
    sys.exit(1)


def parse(argv):
    vintage = False
    words = []
    for a in argv:
        if a == "--vintage-tools":
            vintage = True
        elif a.startswith("-"):
            usage()			# a mistyped option is never a territory
        else:
            words.append(a.lower())
    if len(words) > 2:
        usage()
    first = words[0] if words else "usa"
    if first in ("debug", "final", "usa-debug", "usa-final", "eur-debug", "eur-final"):
        if len(words) > 1:
            usage()
        territory = "EUR" if first.startswith("eur-") else "USA"
        print("note: '%s' -> %s (the data is per territory; the variant word is ignored)" % (words[0], territory))
    elif first in ("usa", "eur"):
        territory = first.upper()
        if len(words) > 1 and words[1] not in ("debug", "final"):
            usage()
    else:
        usage()
    return territory, vintage


def check_tools():
    for t in TOOLS:
        exe = os.path.join(ROOT, "port", "tools", "x64", t + ".exe")
        if not os.path.isfile(exe) or os.path.getsize(exe) < 4096:
            sys.exit("ERROR: port/tools/x64/%s.exe is missing or an unmaterialised Git LFS pointer.\n"
                     "       Run: git lfs pull --include \"port/tools/x64/*.exe\"\n"
                     "       (or add --vintage-tools to build with the 1999 converters)" % t)


def main(argv):
    sys.stdout.reconfigure(line_buffering=True)		# before CMake's output, also into a log
    territory, vintage = parse(argv)
    preset = "data-" + territory.lower()
    print("Data build: TERRITORY=%s -> out/%s" % (territory, territory))
    if vintage:
        print("Converters: the 1999 Win32 exes (--vintage-tools)")
    else:
        check_tools()
        print("Converters: port/tools/x64 (%s)" % ", ".join(TOOLS))

    # Until issue #15 three makefile.gfx recipes redirected to `nul`, which
    # MSYS2's sh made a real file that Explorer and del cannot remove; trees
    # built before that still have one at the root.
    if "nul" in os.listdir(ROOT):
        os.remove("\\\\?\\" + os.path.join(ROOT, "nul"))

    # MkActor's system("lznp.exe ...") needs cmd.exe, found through COMSPEC,
    # which a shell started without a console can lack
    env = dict(os.environ)
    env.setdefault("COMSPEC", os.path.join(os.environ.get("SYSTEMROOT", r"C:\Windows"), "System32", "cmd.exe"))

    cmake = buildenv.find_cmake()
    ninja = buildenv.find_ninja()
    src = os.path.join(ROOT, "port", "data")
    configure = [cmake, "--preset", preset, "-DCMAKE_MAKE_PROGRAM=" + ninja.replace("\\", "/"),
                 "-DSBSP_DATA_TOOLS=" + ("vintage" if vintage else "x64"), "-DSBSP_DATA_FINAL_DAT="]
    for cmd in (configure, [cmake, "--build", "--preset", preset]):
        r = subprocess.run(cmd, cwd=src, env=env)
        if r.returncode != 0:
            return r.returncode
    print("Data build complete: out/%s (PC data: out/%s/cd)" % (territory, territory))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
