#!/usr/bin/env python3
"""M8 playthrough harness: Tier 1 skeleton traversal, Tier 2 content sweep,
and the exit-code self-test, over the PC build of the game.

    run_tier.py --exe port/build/debug/sbsp.exe --tier1 [--fast]
    run_tier.py --exe ... --tier2 [--short]
    run_tier.py --exe ... --selftest

Every run uses the determinism set (--uncapped --no-cd-pace --no-audio
--seed N --frame-crc) plus --record-pad into a temp file; a passing Tier 1
route is then replayed from that recording with the same seed, which checks
the recording's "# epoch" ram/CRC markers and requires identical [scene] and
[frame] streams - the determinism proof, per route (--no-replay skips it).

Tier 1 routes live in port/tests/routes/<name>.pad.  A route's header is
plain pad-file comments the game ignores and this script reads:

    # env SBSP_AUTOPLAY=finish=60          environment for the run
    # args --level 2-5                     extra command-line arguments
    # exit-after 20000                     vblank budget (default 20000)
    # expect Game                          the exact [scene] sequence, in order
    # expect Map
    # max peak_ram=1500000                 [summary] field ceilings
    # timeout 600                          wall-clock seconds (default 900)

Oracle per route: exit code 0, the exact [scene] sequence, none of the
forbidden log lines ([assert] [crash] [watchdog] [replay] [mem] LEAK
[mem] WARNING [gpu] WARNING, and anything tagged [spu] [xm] [xa] [mcrd]),
and every "# max" ceiling.  A failing route prints the scene diff and the
offending lines; the script exits 13 if anything failed.

Tier 2 boots every LvlTable level (--level 0..24) with --invincible and the
shared routes/walk_right.pad, and requires exit 0, no forbidden lines and
at least two distinct [frame] CRCs after the level opened (a black or stuck
display yields exactly one).  Prim/RAM peaks are reported, not judged.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROUTES = HERE / "routes"
REPO = HERE.parents[1]

FORBIDDEN = [
    re.compile(r"^\[assert\]"),
    re.compile(r"^\[crash\]"),
    re.compile(r"^\[watchdog\]"),
    re.compile(r"^\[replay\]"),
    re.compile(r"^\[mem\] LEAK"),
    re.compile(r"^\[mem\] WARNING"),
    re.compile(r"^\[gpu\] WARNING"),
    re.compile(r"^\[(spu|xm|xa|mcrd)\]"),
]

# benign lines that share a forbidden tag
ALLOWED = [
    re.compile(r"^\[mcrd\] created card image "),   # every run gets a fresh --save-dir
]

DETERMINISM = ["--uncapped", "--no-cd-pace", "--no-audio", "--frame-crc"]

FAST_ROUTES = ["campaign", "pause_quit"]
TIER2_SHORT_LEVELS = [0, 4, 12, 19, 24]
EXIT_ORACLE = 13


class Route:
    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.stem
        self.env = {}
        self.args = []
        self.exit_after = 20000
        self.expect = []
        self.max = {}
        self.timeout = 900
        for line in self.path.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if not s.startswith("#"):
                continue
            words = s[1:].split(None, 1)
            if not words:
                continue
            key = words[0]
            val = words[1].strip() if len(words) > 1 else ""
            if key == "env" and "=" in val:
                k, v = val.split("=", 1)
                self.env[k] = v
            elif key == "args":
                self.args += val.split()
            elif key == "exit-after":
                self.exit_after = int(val)
            elif key == "expect":
                self.expect.append(val)
            elif key == "max":
                k, v = val.split("=", 1)
                self.max[k] = int(v)
            elif key == "timeout":
                self.timeout = int(val)


class RunResult:
    def __init__(self, code, lines, wall):
        self.code = code
        self.lines = lines
        self.wall = wall
        self.scenes = [l.split()[1] for l in lines if l.startswith("[scene] ")]
        self.summary = {}
        for l in lines:
            if l.startswith("[summary]"):
                for kv in l.split()[1:]:
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        self.summary[k] = v
        self.forbidden = [l for l in lines
                          if any(p.match(l) for p in FORBIDDEN) and not any(p.match(l) for p in ALLOWED)]

    def frame_crcs_after(self, scene):
        seen = False
        crcs = set()
        for l in self.lines:
            if l.startswith("[scene] ") and l.split()[1] == scene:
                seen = True
            elif seen and l.startswith("[frame] ") and "masked" not in l:
                crcs.add(l.split()[2])
        return crcs

    def summary_int(self, key):
        v = self.summary.get(key, "0").split("/")[0]
        return int(v)


def run_game(exe, args, env, timeout, log_path=None):
    full_env = dict(os.environ)
    full_env.update(env)
    # a private memory-card directory: the boot autoload must not see the
    # user's real card0.mcd, and a route must start from empty slots
    save_dir = tempfile.mkdtemp(prefix="sbsp_save_")
    cmd = [str(exe), "--save-dir", save_dir] + args
    t0 = time.time()
    # The harness lines are all stderr; the game's own printf debug text goes
    # to stdout and, block-buffered through a pipe, would splice itself into
    # the middle of stderr lines if the two were merged.
    try:
        p = subprocess.run(cmd, cwd=str(REPO), env=full_env, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = p.stderr.decode("utf-8", "replace")
        game_out = p.stdout.decode("utf-8", "replace")
        code = p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stderr or b"").decode("utf-8", "replace") + "\n[run_tier] TIMEOUT\n"
        game_out = (e.stdout or b"").decode("utf-8", "replace")
        code = -1
    wall = time.time() - t0
    if log_path:
        Path(log_path).write_text(out, encoding="utf-8")
        Path(str(log_path) + ".stdout").write_text(game_out, encoding="utf-8")
    for f in Path(save_dir).glob("*"):
        f.unlink()
    os.rmdir(save_dir)
    return RunResult(code, out.splitlines(), wall)


def report_common(res, name):
    ok = True
    if res.code != 0:
        print(f"  FAIL {name}: exit code {res.code} (0 expected)")
        ok = False
    if res.forbidden:
        print(f"  FAIL {name}: {len(res.forbidden)} forbidden log line(s):")
        for l in res.forbidden[:20]:
            print("       " + l)
        ok = False
    return ok


def run_route(exe, route, seed, logdir, replay):
    with tempfile.NamedTemporaryFile(prefix=f"{route.name}_", suffix=".rec.pad", delete=False) as tmp:
        rec = tmp.name
    args = ["--pad-file", str(route.path), "--record-pad", rec, "--seed", str(seed),
            "--exit-after", str(route.exit_after)] + DETERMINISM + route.args
    log = Path(logdir) / f"{route.name}.log" if logdir else None
    print(f"== tier1 {route.name}: {' '.join(args)}"
          + (f"  env {route.env}" if route.env else ""))
    res = run_game(exe, args, route.env, route.timeout, log)
    ok = report_common(res, route.name)
    if res.scenes != route.expect:
        print(f"  FAIL {route.name}: [scene] sequence differs (expected vs actual):")
        for l in difflib.unified_diff(route.expect, res.scenes, "expected", "actual", lineterm="", n=2):
            print("       " + l)
        ok = False
    for k, ceiling in route.max.items():
        v = res.summary_int(k)
        if v > ceiling:
            print(f"  FAIL {route.name}: {k}={v} exceeds {ceiling}")
            ok = False
    print(f"  {'PASS' if ok else 'FAIL'} {route.name}: {res.wall:.1f}s wall, "
          f"{res.summary.get('vblanks', '?')} vblanks, {len(res.scenes)} scenes, "
          f"peak_ram={res.summary.get('peak_ram', '?')} peak_prim={res.summary.get('peak_prim', '?')} "
          f"peak_memnodes={res.summary.get('peak_memnodes', '?')}")
    if ok and replay:
        # Replay the recording with the same seed: input now comes from the
        # recorded scene-relative entries, its "# epoch" ram/CRC markers are
        # checked, and the [scene]/[frame] streams must match the first run
        # - so a wall-clock leak or an uninitialised read that survives the
        # determinism set shows up here as a desync.
        args = ["--pad-file", rec, "--seed", str(seed),
                "--exit-after", str(route.exit_after)] + DETERMINISM + route.args
        log2 = Path(logdir) / f"{route.name}.replay.log" if logdir else None
        res2 = run_game(exe, args, route.env, route.timeout, log2)
        ok = report_common(res2, route.name + " (replay)")
        if res2.scenes != res.scenes:
            print(f"  FAIL {route.name} (replay): [scene] sequence differs from the recorded run")
            ok = False
        frames1 = [l for l in res.lines if l.startswith("[frame] ")]
        frames2 = [l for l in res2.lines if l.startswith("[frame] ")]
        if frames1 != frames2:
            print(f"  FAIL {route.name} (replay): [frame] CRC stream differs from the recorded run")
            ok = False
        epochs = sum(1 for l in Path(rec).read_text(encoding="utf-8").splitlines() if l.startswith("# epoch"))
        print(f"  {'PASS' if ok else 'FAIL'} {route.name} (replay): {res2.wall:.1f}s wall, "
              f"{len(frames1)} frames compared, {epochs} epochs checked")
    if ok:
        os.unlink(rec)
    else:
        print(f"       recording kept: {rec}")
    return ok


def tier1(exe, seed, fast, logdir, only, replay):
    names = FAST_ROUTES if fast else sorted(p.stem for p in ROUTES.glob("*.pad") if p.stem != "walk_right")
    if only is not None:
        names = only
        if not names:
            print("  tier 1: nothing selected by --only")
            return True
    ok = True
    for n in names:
        path = ROUTES / f"{n}.pad"
        if not path.exists():
            print(f"  FAIL {n}: route file missing: {path}")
            ok = False
            continue
        ok &= run_route(exe, Route(path), seed, logdir, replay)
    return ok


def tier2(exe, seed, short, logdir, only):
    levels = TIER2_SHORT_LEVELS if short else list(range(25))
    if only is not None:
        levels = only
        if not levels:
            print("  tier 2: nothing selected by --only")
            return True
    budget = 900 if short else 3600
    walk = ROUTES / "walk_right.pad"
    ok = True
    for lvl in levels:
        name = f"level{lvl}"
        args = ["--level", str(lvl), "--invincible", "--pad-file", str(walk), "--seed", str(seed),
                "--exit-after", str(budget)] + DETERMINISM
        log = Path(logdir) / f"tier2_{name}.log" if logdir else None
        print(f"== tier2 {name} (chapter {lvl // 5 + 1} level {lvl % 5 + 1}): {' '.join(args)}")
        res = run_game(exe, args, {}, 600, log)
        good = report_common(res, name)
        crcs = res.frame_crcs_after("Game")
        if len(crcs) < 2:
            print(f"  FAIL {name}: only {len(crcs)} distinct unmasked frame CRC(s) after [scene] Game")
            good = False
        print(f"  {'PASS' if good else 'FAIL'} {name}: {res.wall:.1f}s wall, {len(crcs)} distinct frames, "
              f"peak_ram={res.summary.get('peak_ram', '?')} peak_prim={res.summary.get('peak_prim', '?')} "
              f"peak_memnodes={res.summary.get('peak_memnodes', '?')}")
        ok &= good
    return ok


def selftest(exe, seed, logdir):
    cases = [
        ("assert", {"SBSP_SELFTEST": "assert@100"}, 10, "[assert]"),
        ("fault", {"SBSP_SELFTEST": "fault@100"}, 11, "[crash]"),
        ("hang", {"SBSP_SELFTEST": "hang@100", "SBSP_WATCHDOG": "3"}, 12, "[watchdog]"),
        ("assert-continue", {"SBSP_SELFTEST": "assert@100", "SBSP_ASSERT_CONTINUE": "1"}, 0, "[assert]"),
    ]
    ok = True
    for name, env, want, tag in cases:
        args = ["--level", "1-1", "--seed", str(seed), "--exit-after", "300"] + DETERMINISM
        log = Path(logdir) / f"selftest_{name}.log" if logdir else None
        res = run_game(exe, args, env, 120, log)
        tagged = any(l.startswith(tag) for l in res.lines)
        summary = any(l.startswith("[summary]") for l in res.lines)
        good = res.code == want and tagged and summary
        print(f"  {'PASS' if good else 'FAIL'} selftest {name}: exit {res.code} (want {want}), "
              f"{tag} {'seen' if tagged else 'MISSING'}, [summary] {'seen' if summary else 'MISSING'}")
        ok &= good
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="sbsp.exe to drive")
    ap.add_argument("--tier1", action="store_true")
    ap.add_argument("--fast", action="store_true", help=f"tier 1: only {FAST_ROUTES}")
    ap.add_argument("--tier2", action="store_true")
    ap.add_argument("--short", action="store_true", help=f"tier 2: levels {TIER2_SHORT_LEVELS}, 900 vblanks")
    ap.add_argument("--selftest", action="store_true", help="exit-code proofs via SBSP_SELFTEST")
    ap.add_argument("--only", nargs="*", help="route names (tier 1) and/or level indices 0-24 (tier 2)")
    ap.add_argument("--no-replay", action="store_true", help="tier 1: skip replaying each route's recording")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--logs", help="directory to keep every run's log")
    a = ap.parse_args()

    exe = Path(a.exe).resolve()
    if not exe.exists():
        print(f"no such exe: {exe}")
        return 2
    if a.logs:
        Path(a.logs).mkdir(parents=True, exist_ok=True)
    if not (a.tier1 or a.tier2 or a.selftest):
        ap.error("nothing to do: pass --tier1, --tier2 and/or --selftest")

    only_routes = only_levels = None
    if a.only is not None:
        only_routes = [x for x in a.only if not x.isdigit()]
        only_levels = [int(x) for x in a.only if x.isdigit()]
        bad = [x for x in only_levels if not 0 <= x <= 24]
        if bad:
            ap.error(f"--only: level index out of range 0-24: {bad}")

    ok = True
    if a.selftest:
        ok &= selftest(exe, a.seed, a.logs)
    if a.tier1:
        ok &= tier1(exe, a.seed, a.fast, a.logs, only_routes, not a.no_replay)
    if a.tier2:
        ok &= tier2(exe, a.seed, a.short, a.logs, only_levels)
    print("ALL PASSED" if ok else "FAILURES")
    return 0 if ok else EXIT_ORACLE


if __name__ == "__main__":
    sys.exit(main())
