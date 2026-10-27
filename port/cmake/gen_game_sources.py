#!/usr/bin/env python3
"""Regenerates port/cmake/game_sources.cmake from makefile.gaz's *_src lists
(the authoritative object manifest of the vintage CD link).  Run after any TU
is added to or removed from makefile.gaz, then commit both files:

    python port/cmake/gen_game_sources.py            write the file
    python port/cmake/gen_game_sources.py --check    exit 1 if it is out of date

The $(FILE_SYSTEM)_FILESYS_SRC indirection resolves to cdfile for the CD
build; system/main.cpp is split out so test exes can supply main().  Issue
#98 ported this from gen_game_sources.sh and extract_tus.awk; the TUs are in
byte order, as `sort` gave them.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "port", "cmake", "game_sources.cmake")

LIST_START = re.compile(r"^[a-z]+_src[ \t]*:=")
CONTINUED = re.compile(r"\\[ \t\r]*$")


def tus(makefile):
    """<dir>/<file> for every word of every *_src list, continuation lines included."""
    out = []
    collecting = False
    folder = ""
    with open(makefile, newline="") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if LIST_START.match(line):
                folder = re.sub(r"_src.*", "", line.split()[0])
                collecting = bool(CONTINUED.search(line))
                words = line.split("=", 1)[1]
            elif collecting:
                collecting = bool(CONTINUED.search(line))
                words = line
            else:
                continue
            for w in words.replace("\\", "").replace("\r", "").split():
                out.append(folder + "/" + w)
    return out


def generate():
    listed = [t for t in tus(os.path.join(ROOT, "makefile.gaz"))
              if "FILESYS_SRC" not in t and "_src_mip" not in t]
    listed = sorted(set(listed + ["fileio/cdfile"]))
    missing = [t for t in listed if not os.path.isfile(os.path.join(ROOT, "source", t + ".cpp"))]
    if missing:
        sys.exit("".join("ERROR: makefile.gaz lists source/%s.cpp but it does not exist\n" % t for t in missing))
    lines = [
        "# Game translation-unit manifest, generated from the *_src lists in makefile.gaz",
        "# by port/cmake/gen_game_sources.py - regenerate with that script, do not edit",
        "# by hand.  %d TUs: all but system/main.cpp in the static lib (main is split" % len(listed),
        "# out so test exes can supply their own main()).",
        "# Dead .cpp files under source/ are deliberately absent - do NOT glob.",
        "",
        "set(SBSP_GAME_MAIN_SOURCE ${SBSP_ROOT}/source/system/main.cpp)",
        "",
        "set(SBSP_GAME_SOURCES",
    ]
    lines += ["    ${SBSP_ROOT}/source/%s.cpp" % t for t in listed if t != "system/main"]
    lines.append(")")
    return "\n".join(lines) + "\n", len(listed) - ("system/main" in listed)


def main(argv):
    text, lib = generate()
    try:
        with open(OUT, newline="") as f:
            current = f.read().replace("\r\n", "\n")
    except OSError:
        current = None
    if "--check" in argv:
        if current != text:
            print("%s is out of date: run python port/cmake/gen_game_sources.py" % OUT, file=sys.stderr)
            return 1
        print("%s is up to date (%d lib TUs + main)" % (OUT, lib))
        return 0
    if current != text:
        with open(OUT, "w", newline="\n") as f:
            f.write(text)
    print("wrote %s (%d lib TUs + main)" % (OUT, lib))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
