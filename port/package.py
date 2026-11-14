#!/usr/bin/env python3
"""Build the tester zip for the PC port (M8 shell, issue #10 PR 3).

    python port/package.py [--territory usa|eur] [--out <zip>] [--build]

Layout of the zip (everything the exes need, nothing else):

    sbsp-<territory>-<date>/
      sbsp.exe                 the FINAL build (64-bit, clang-cl)
      sbsp-debug.exe           the DEBUG build (asserts live; test sessions use it)
      SDL3.dll                 the exes' SDL (the official SDL VC package has
                               no static lib)
      run-test-session.cmd     one recorded session: snapshots the card, runs
                               sbsp-debug.exe --record-pad --assert-continue
                               --mem-log, keeps stdout/stderr apart
      README.txt               controls, settings, what to send back
      data/                    BIGLUMP.BIN TRACK1.IXA THQ.STR CLIMAX.STR INTRO.STR DEMO.STR
      saves/                   empty; the exe finds it beside itself and keeps
                               card0.mcd there (portable; sbsp.ini is written
                               beside the exe)

The exe locates data/ and saves/ next to itself (port/psyq/cd/cd.cpp,
host/hostpath.cpp), so the folder can live anywhere and needs no
installation.  Until issue #39 the zip held 32-bit MinGW exes and, with
--x64, a 64-bit pair beside them (sbsp64.exe); the x64 exes are now the
only ones, under the plain names.  --x64 is still accepted and changes
nothing.

Inputs are the repo's own build products - port/build/<preset>/sbsp.exe and
SDL3.dll for the territory's two x64 presets and out/<T>/cd/* from
port/build-data.cmd <T>.
Missing inputs are reported with the exact command that makes them;
--build runs those commands first.  TRACK1.IXA is Git-LFS tracked, so an
unmaterialised pointer file (a few hundred bytes) is refused.
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent          # port/
REPO = HERE.parent
PACKAGE_DIR = HERE / "package"                  # README.txt, run-test-session.cmd

DATA_FILES = ["BIGLUMP.BIN", "TRACK1.IXA", "THQ.STR", "CLIMAX.STR", "INTRO.STR", "DEMO.STR"]
RAW_XA = {"TRACK1.IXA", "THQ.STR", "CLIMAX.STR", "INTRO.STR", "DEMO.STR"}   # 2336-byte sectors
# the clang-cl x64 trees (port/build-pc.cmd usa | eur), and the DLL
# sbsp_exe_link copies beside each of their exes
PRESETS = {"usa": ("final", "debug"), "eur": ("eur-final", "eur-debug")}
DLLS = ["SDL3.dll"]
PE_MACHINE = {0x014C: "x86", 0x8664: "x64"}


def pe_machine(path):
    """'x86' / 'x64' from the PE header, or None if the file is not a PE at
    all - the zip must not carry a 32-bit SDL3.dll beside the 64-bit exes, or
    an exe from the wrong tree.  "not a PE" is its own answer rather than a
    string in the same channel: the caller names the architecture it wanted
    in the message, and a sentence there would read as one."""
    with open(path, "rb") as f:
        head = f.read(4096)
    off = int.from_bytes(head[0x3C:0x40], "little")
    if head[:2] != b"MZ" or head[off:off + 2] != b"PE":
        return None
    code = int.from_bytes(head[off + 4:off + 6], "little")
    return PE_MACHINE.get(code, f"machine 0x{code:04X}")


def die(msg):
    print(f"package: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd):
    print("+", " ".join(cmd))
    if subprocess.call(cmd, cwd=str(REPO)) != 0:
        die(f"{cmd[0]} failed")


def preflight(territory, build):
    terr = territory.upper()
    final, debug = PRESETS[territory]
    exes = {name: HERE / "build" / preset / "sbsp.exe" for name, preset in (("sbsp.exe", final), ("sbsp-debug.exe", debug))}
    for dll in DLLS:          # one copy serves both exes; take the FINAL tree's
        exes[dll] = HERE / "build" / final / dll
    data_dir = REPO / "out" / terr / "cd"

    if build:
        run([str(HERE / "build-data.cmd"), territory])
        run([str(HERE / "build-pc.cmd"), territory])

    problems = []
    for name, path in exes.items():
        if not path.is_file():
            problems.append(f"missing {path.relative_to(REPO)} - run: port\\build-pc.cmd {territory}")
            continue
        got = pe_machine(path)
        if got is None:
            problems.append(f"{path.relative_to(REPO)} is not a PE file - truncated, or a "
                            f"half-written copy? run: port\\build-pc.cmd {territory}")
        elif got != "x64":
            # the trees named debug/final/eur-* were 32-bit MinGW before #39
            problems.append(f"{path.relative_to(REPO)} is {got}, not x64 - "
                            f"a stale tree? run: port\\build-pc.cmd {territory}")
    for name in DATA_FILES:
        path = data_dir / name
        if not path.is_file():
            problems.append(f"missing {path.relative_to(REPO)} - run: port\\build-data.cmd {territory}")
            continue
        size = path.stat().st_size
        if size < 1 << 20:
            problems.append(f"{path.relative_to(REPO)} is {size} bytes - an unmaterialised Git-LFS pointer? "
                            f"run: git lfs pull, then port\\build-data.cmd {territory}")
        elif name in RAW_XA and size % 2336:
            problems.append(f"{path.relative_to(REPO)}: {size} bytes is not a whole number of 2336-byte sectors")
    if problems:
        for p in problems:
            print("package:", p, file=sys.stderr)
        sys.exit(1)
    return exes, data_dir


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--territory", choices=sorted(PRESETS), default="usa")
    ap.add_argument("--out", help="zip path (default port/build/sbsp-<territory>-<yyyymmdd>.zip)")
    ap.add_argument("--build", action="store_true", help="run build-data.cmd and build-pc.cmd for the territory first")
    # the zip's exes are x64 since issue #39; --x64 used to add them
    ap.add_argument("--x64", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    exes, data_dir = preflight(args.territory, args.build)
    for f in ("README.txt", "run-test-session.cmd"):
        if not (PACKAGE_DIR / f).is_file():
            die(f"missing {PACKAGE_DIR / f}")

    name = f"sbsp-{args.territory}-{datetime.date.today():%Y%m%d}"
    out = Path(args.out) if args.out else HERE / "build" / f"{name}.zip"
    stage = HERE / "build" / "package" / name
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "data").mkdir(parents=True)
    (stage / "saves").mkdir()

    for dst_name, src in exes.items():
        shutil.copy2(src, stage / dst_name)
    for f in DATA_FILES:
        shutil.copy2(data_dir / f, stage / "data" / f)
    for f in ("README.txt", "run-test-session.cmd"):
        # CRLF whatever the checkout did: cmd.exe wants it for batch files
        # and every Windows editor is happier with it
        text = (PACKAGE_DIR / f).read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        (stage / f).write_bytes(text)

    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(stage.rglob("*")):
            rel = Path(name) / path.relative_to(stage)
            if path.is_dir():
                z.writestr(str(rel).replace(os.sep, "/") + "/", "")   # keep saves/ even though empty
            else:
                z.write(path, str(rel).replace(os.sep, "/"))
    size = out.stat().st_size
    print(f"package: {out} ({size / (1 << 20):.1f} MB, {args.territory.upper()} FINAL + DEBUG, x64, "
          f"staged in {stage.relative_to(REPO)})")


if __name__ == "__main__":
    main()
