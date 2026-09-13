#!/usr/bin/env python3
"""Build the tester zip for the PC port (M8 shell, issue #10 PR 3).

    python port/package.py [--territory usa|eur] [--out <zip>] [--build]

Layout of the zip (everything the exe needs, nothing else - the exes are
fully static, no DLLs):

    sbsp-<territory>-<date>/
      sbsp.exe                 the FINAL build
      sbsp-debug.exe           the DEBUG build (asserts live; test sessions use it)
      run-test-session.cmd     one recorded session: snapshots the card, runs
                               sbsp-debug.exe --record-pad --assert-continue
                               --mem-log, keeps stdout/stderr apart
      README.txt               controls, settings, what to send back
      data/                    BIGLUMP.BIN TRACK1.IXA THQ.STR CLIMAX.STR INTRO.STR DEMO.STR
      saves/                   empty; the exe finds it beside itself and keeps
                               card0.mcd and sbsp.ini there (portable)

The exe locates data/ and saves/ next to itself (port/psyq/cd/cd.cpp,
host/hostpath.cpp), so the folder can live anywhere and needs no
installation.

Inputs are the repo's own build products - port/build/<preset>/sbsp.exe for
the territory's two presets and out/<T>/cd/* from port/build-data.cmd <T>.
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
PRESETS = {"usa": ("final", "debug"), "eur": ("eur-final", "eur-debug")}


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
    data_dir = REPO / "out" / terr / "cd"

    if build:
        run([str(HERE / "build-data.cmd"), territory])
        run([str(HERE / "build-pc.cmd"), territory])

    problems = []
    for name, path in exes.items():
        if not path.is_file():
            problems.append(f"missing {path.relative_to(REPO)} - run: port\\build-pc.cmd {territory}")
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
    print(f"package: {out} ({size / (1 << 20):.1f} MB, {args.territory.upper()} FINAL + DEBUG, "
          f"staged in {stage.relative_to(REPO)})")


if __name__ == "__main__":
    main()
