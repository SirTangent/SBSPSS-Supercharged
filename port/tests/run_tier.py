#!/usr/bin/env python3
"""M8 playthrough harness: Tier 1 skeleton traversal, Tier 2 content sweep,
and the exit-code self-test, over the PC build of the game.

    run_tier.py --exe port/build/debug/sbsp.exe --tier1 [--fast]
    run_tier.py --exe port/build/eur-debug/sbsp.exe --territory EUR --tier1
    run_tier.py --exe ... --tier2 [--short]
    run_tier.py --exe ... --selftest

Every run uses the determinism set (--uncapped --no-cd-pace --no-audio
--seed N --frame-crc) plus --record-pad into a temp file; a passing Tier 1
route is then replayed from that recording with the same seed, which checks
the recording's "# epoch" ram/CRC markers and requires identical [scene] and
[frame] streams - the determinism proof, per route (--no-replay skips it).
--no-cd-pace keeps every baseline independent of load pacing; the self-test
adds one pair of paced runs, which must agree too (issue #67).

Runs are isolated from the machine (issue #58): every inherited SBSP_*
variable is dropped (a route's "# env" is the whole environment), and every
run is a scripted-input run (--pad-file, or for the self-test a one-entry
--pad-script), for which the game reads no sbsp.ini and ignores its live
keyboard and pad - so a local ini or a plugged-in controller cannot move a
frame CRC.  A "[ini] loaded" line in a run's log is therefore a failure.

Tier 1 routes live in port/tests/routes/<name>.pad.  A route's header is
plain pad-file comments the game ignores and this script reads:

    # env SBSP_AUTOPLAY=finish=60          environment for the run
    # args --level 2-5                     extra command-line arguments
    # exit-after 20000                     vblank budget (default 20000)
    # expect Game                          the exact [scene] sequence, in order
    # expect Map
    # max peak_ram=1500000                 [summary] field ceilings
    # timeout 600                          wall-clock seconds (default 900)
    # min-frames 150                       distinct unmasked [frame] CRCs required
    # territory EUR                        the route runs only for that --territory
    # eur max peak_prim=70000              any key, applied only for that
    # usa exit-after 12000                 --territory (later lines win)

--territory USA|EUR names the exe's build (the EUR build runs at 50Hz with
the PAL frontend, which has the PLAY TRAILER menu item); it selects the
"# territory" routes and the territory-prefixed header lines, and adds
that territory's extra routes to --fast.  Every other header line and
every route body is shared: scene-relative offsets count frames, so a
route authored on USA replays on EUR unchanged.

Oracle per route: exit code 0, the exact [scene] sequence, none of the
forbidden log lines ([assert] [crash] [watchdog] [replay] [mem] LEAK
[mem] WARNING [gpu] WARNING, and anything tagged [spu] [xm] [xa] [mcrd]),
every "# max" ceiling and the "# min-frames" floor.  A failing route
prints the scene diff and the offending lines; the script exits 13 if
anything failed.

Tier 2 boots every LvlTable level (--level 0..24) with --invincible and the
shared routes/walk_right.pad, and requires exit 0, no forbidden lines and
at least two distinct [frame] CRCs after the level opened (a black or stuck
display yields exactly one).  Prim/RAM peaks are reported, not judged.

--compare-frames DIR is the cross-build pixel oracle (M8 perf pass): DIR is
the --logs directory of an earlier run of the same tiers on another build
of the same territory/variant, and each run's [scene] + [frame] stream must
match its same-named log there line for line.

    run_tier.py --exe <before> --tier1 --tier2 --logs base
    run_tier.py --exe <after>  --tier1 --tier2 --compare-frames base

--keep-artifacts DIR / --replay-from DIR is the cross-exe replay (M9, the
x64 A/B): the first exe keeps each Tier 1 route's recording and memory card,
the second replays those recordings instead of the routes and must produce
no [replay] desync, the first exe's streams and a byte-identical card.

    run_tier.py --exe <x86> --tier1 --logs L32 --keep-artifacts A32
    run_tier.py --exe <x64> --tier1 --logs L64 --compare-frames L32 --replay-from A32

When the recording exe drew with another renderer revision (the recording's
`# render`, issue #60, as the second exe reads it and says at boot), the
second exe draws the same game state differently, so only the [scene]
lines are held to the first exe's log there; the [frame] lines are not
compared, and the replay's own epochs still hold rng and ram where they can.
Every run's [summary] blind_epochs must be 0: an epoch that compared
nothing proves nothing.
"""
import argparse
import difflib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
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
    re.compile(r"^\[ini\] loaded"),      # a scripted run must read no sbsp.ini (issue #58)
]

# benign lines that share a forbidden tag
ALLOWED = [
    re.compile(r"^\[mcrd\] created card image "),   # every run gets a fresh --save-dir
]

# --no-audio used to be here: with no consumer the SPU mixer never ran, so
# the voice render, the envelopes, ADPCM over the game's own VABs and the
# CD-in mix had no playthrough coverage at all (issue #61).  run_game adds a
# --dump-audio into each run's private directory instead - no device, and
# sample-exact (the M5 contract).
DETERMINISM = ["--uncapped", "--no-cd-pace", "--frame-crc"]

# Tier 2's frame oracle: the level's fade-in alone gives ~25 distinct frames,
# so a count over the whole run cannot tell a level that wedges after opening
# from one that plays (issue #61).  The TIER2_LATE_WINDOW vblanks starting
# TIER2_LATE_START after [scene] Game - well past the fade - must show at
# least TIER2_LATE_FLOOR distinct pictures: the walk-right sweep scrolls,
# animates and jumps, and over the full tier's 25 levels the quietest (7)
# gives 132 there, the rest 136-300.  Not the run's tail: on the long budget
# walk-right reaches the exit of levels 9, 19 and 24 and the game then sits
# on the map, 8 distinct frames in the last 300.  A level whose Game scene
# ends before the window closes has played through and passes.
TIER2_LATE_START = 300
TIER2_LATE_WINDOW = 300
TIER2_LATE_FLOOR = 30

FAST_ROUTES = ["campaign", "pause_quit"]
FAST_ROUTES_EXTRA = {"EUR": ["play_trailer"]}   # territory-only routes worth the ctest budget
TERRITORIES = ("USA", "EUR")
TIER2_SHORT_LEVELS = [0, 4, 12, 19, 24]
EXIT_ORACLE = 13


class Route:
    def __init__(self, path, territory="USA"):
        self.path = Path(path)
        self.name = self.path.stem
        self.env = {}
        self.args = []
        self.exit_after = 20000
        self.expect = []
        self.max = {}
        self.timeout = 900
        self.min_frames = 0
        self.territory = None          # "# territory EUR": runs only for that build
        for line in self.path.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if not s.startswith("#"):
                continue
            words = s[1:].split(None, 1)
            if not words:
                continue
            key = words[0]
            val = words[1].strip() if len(words) > 1 else ""
            # "# eur max peak_prim=70000": the rest of the line is an ordinary
            # header key that applies to that territory only
            if key.upper() in TERRITORIES:
                if key.upper() != territory:
                    continue
                words = val.split(None, 1)
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
            elif key == "min-frames":
                self.min_frames = int(val)
            elif key == "territory":
                self.territory = val.upper()


# run_game's summary of the audio a run rendered (see audio_line)
AUDIO_LINE = re.compile(r"^\[audio\] frames=(\d+) crc=([0-9A-F]{8})$")


class RunResult:
    def __init__(self, code, lines, wall, card=None):
        self.code = code
        self.lines = lines
        self.wall = wall
        # bytes of the card0.mcd the run left, or None if it never saved
        self.card = card
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
        # the "[audio] frames=<n> crc=<hex>" line run_game appends; None when
        # the run brought its own --dump-audio (selftest_wav) and so has none
        self.audio = None
        for l in lines:
            m = AUDIO_LINE.match(l)
            if m:
                self.audio = (int(m.group(1)), m.group(2))
            elif l.startswith("[audio] missing"):
                self.audio = "missing"

    def frame_crcs(self, after_scene=None):
        """distinct unmasked [frame] CRCs - over the whole run, or only after
        the first open of `after_scene`"""
        seen = after_scene is None
        crcs = set()
        for l in self.lines:
            if l.startswith("[scene] ") and l.split()[1] == after_scene:
                seen = True
            elif seen and l.startswith("[frame] ") and "masked" not in l:
                crcs.add(l.split()[2])
        return crcs

    def frame_crcs_after_open(self, scene, start, end):
        """(distinct unmasked [frame] CRCs over vblanks open+start < v <= open+end
        after the first open of `scene`, whether another scene opened by
        open+end) - (set(), False) when the scene never opened"""
        opens = [int(l.split("vblank=")[1]) for l in self.lines
                 if l.startswith("[scene] ") and "vblank=" in l]
        mine = [int(l.split("vblank=")[1]) for l in self.lines
                if l.startswith("[scene] ") and l.split()[1] == scene]
        if not mine:
            return set(), False
        open_at = mine[0]
        ended = any(open_at < v <= open_at + end for v in opens)
        crcs = {l.split()[2] for l in self.lines
                if l.startswith("[frame] ") and "masked" not in l
                and open_at + start < int(l.split()[1]) <= open_at + end}
        return crcs, ended

    def summary_int(self, key):
        """an integer [summary] field, or None when the line or the key is
        missing - read as 0, a renamed peak_prim= would have held every
        route's `# max` ceiling against nothing (issue #61)"""
        v = self.summary.get(key)
        return int(v.split("/")[0]) if v is not None else None


def audio_line(wav):
    """the run's WAV as one log line, "[audio] frames=<n> crc=<CRC-32 of
    the PCM>", or "[audio] missing" when no header was ever written"""
    try:
        b = wav.read_bytes()
    except OSError:
        b = b""
    if len(b) < 44 or b[0:4] != b"RIFF" or b[36:40] != b"data":
        return "[audio] missing"
    data, = struct.unpack_from("<I", b, 40)
    pcm = b[44:44 + data]
    return f"[audio] frames={len(pcm) // 4} crc={zlib.crc32(pcm) & 0xFFFFFFFF:08X}"


def run_game(exe, args, env, timeout, log_path=None):
    # Nothing of the caller's SBSP_* configuration reaches the game - an
    # exported SBSP_PROMPT_ICONS, SBSP_KEY_*, SBSP_LANGUAGE or SBSP_DATA_DIR
    # would change what is drawn or loaded while the run still reported
    # PASS.  The rest of the environment stays: CI's SDL_VIDEODRIVER=dummy
    # is what makes the runner headless.
    full_env = {k: v for k, v in os.environ.items() if not k.upper().startswith("SBSP_")}
    full_env.update(env)
    # a private memory-card directory: the boot autoload must not see the
    # user's real card0.mcd, and a route must start from empty slots
    save_dir = tempfile.mkdtemp(prefix="sbsp_save_")
    cmd = [str(exe), "--save-dir", save_dir] + args
    # Every run renders audio (issue #61): a --dump-audio into the private
    # directory opens no device and is sample-exact, so the SPU voices, the
    # envelopes, ADPCM over the game's own VABs and the CD-in mix run every
    # vblank - under --no-audio none of it ever ran here.  The WAV becomes
    # one "[audio] frames=<n> crc=<hex>" log line (audio_line), which
    # report_common checks and the baseline compare holds like a frame.
    wav = None
    if "--dump-audio" not in args:
        wav = Path(save_dir) / "audio.wav"
        cmd += ["--dump-audio", str(wav)]
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
    if wav is not None:
        if out and not out.endswith("\n"):
            out += "\n"
        out += audio_line(wav) + "\n"
    if log_path:
        Path(log_path).write_text(out, encoding="utf-8")
        Path(str(log_path) + ".stdout").write_text(game_out, encoding="utf-8")
    # the memory card the run left behind (None: it never saved), for
    # --keep-artifacts / --replay-from
    card = Path(save_dir) / "card0.mcd"
    card_bytes = card.read_bytes() if card.exists() else None
    shutil.rmtree(save_dir, ignore_errors=True)
    return RunResult(code, out.splitlines(), wall, card_bytes)


BASELINE = None     # --compare-frames: directory of an earlier run's --logs


def compare_baseline(res, name, log_name, scenes_only=False):
    """--compare-frames: this run's [scene] + [frame] lines must equal those
    of the same-named log another build left with --logs.  Everything here
    runs under the determinism set, so the streams are a pure function of
    the exe: a rasterizer (or any other) change that moves one displayed
    pixel on one vblank shows up as the first differing line.  scenes_only
    holds the [scene] lines alone (a cross replay across renderer
    revisions, run_route_cross)."""
    if BASELINE is None:
        return True
    path = Path(BASELINE) / log_name
    if not path.exists():
        print(f"  FAIL {name}: no baseline log {path}")
        return False
    base_lines = path.read_text(encoding="utf-8").splitlines()
    # the "[audio]" line joins the stream when the baseline carries one (a
    # log from before issue #61 has none); audio does not depend on the
    # renderer, so it is held across renderer revisions too
    audio = any(l.startswith("[audio] ") for l in base_lines)
    tags = ("[scene] ",) if scenes_only else ("[frame] ", "[scene] ")
    what = "[scene]" if scenes_only else "[scene]/[frame]"
    if audio:
        tags += ("[audio] ",)
        what += "/[audio]"
    def stream(lines):
        return [l for l in lines if l.startswith(tags)]
    want = stream(base_lines)
    got = stream(res.lines)
    # An empty stream on BOTH sides compares equal, so a baseline captured
    # from a build that emitted no [frame] lines at all - the determinism set
    # lost --frame-crc, the log was truncated - would pass every run against
    # every other.  The oracle has to have something to say.
    if not want:
        print(f"  FAIL {name}: baseline {path} has no {what} lines to compare against")
        return False
    if want == got:
        print(f"       baseline: {len(got)} {what} lines identical to {path}")
        return True
    for i, (w, g) in enumerate(zip(want, got)):
        if w != g:
            print(f"  FAIL {name}: differs from baseline {path} at stream line {i + 1}:")
            print(f"       baseline: {w}")
            print(f"       this run: {g}")
            return False
    print(f"  FAIL {name}: baseline {path} has {len(want)} {what} lines, this run {len(got)}")
    return False


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
    # An epoch that compared nothing (no rng, ram and crc both skipped) is no
    # desync, so the game only refuses a replay made entirely of them; the
    # harness accepts none (host/input.cpp, issue #76's review).
    # The [summary] line is the contract every ceiling and count below reads,
    # so a run that ends without one cannot pass; nor can one whose line
    # lacks a field (summary_int is None then, never 0 - issue #61).
    if not res.summary:
        print(f"  FAIL {name}: no [summary] line")
        return False
    blind = res.summary_int("blind_epochs")
    if blind is None:
        print(f"  FAIL {name}: no blind_epochs= in [summary]")
        ok = False
    elif blind:
        print(f"  FAIL {name}: {blind} epoch(s) compared nothing ([summary] blind_epochs)")
        ok = False
    # The audio the run rendered (run_game's --dump-audio): the exit hook
    # closed the WAV, and it holds exactly one vblank of 44.1 kHz stereo for
    # every vblank the run delivered (the dump is armed at the first vblank,
    # audio_out.cpp): 735 frames at 60 Hz, 882 at 50.  A PAL build boots at
    # 60 Hz until VidInit sets the video mode - a paced boot spends ~34
    # vblanks loading before that - so the count is k * 735 + (V - k) * 882
    # for some whole k in 0..V, i.e. k = (882 V - F) / 147 (k = V on USA).
    if res.code == 0 and res.audio is not None:
        vblanks = res.summary_int("vblanks")
        if res.audio == "missing":
            print(f"  FAIL {name}: the run left no audio dump")
            ok = False
        else:
            frames, crc = res.audio
            closed = [l for l in res.lines if l.startswith("[host] audio dump closed:")]
            m = re.match(r"\[host\] audio dump closed: (\d+) frames", closed[-1] if closed else "")
            if not m or int(m.group(1)) != frames:
                print(f"  FAIL {name}: the exit hook did not close the audio dump at {frames} frames "
                      f"({closed[-1] if closed else 'no [host] audio dump closed line'})")
                ok = False
            k, rem = divmod(882 * vblanks - frames, 147) if vblanks is not None else (-1, 1)
            if rem or not 0 <= k <= vblanks:
                print(f"  FAIL {name}: the audio dump holds {frames} frames, not a mix of whole "
                      f"vblanks at 735 and 882 adding up to vblanks={vblanks}")
                ok = False
            else:
                print(f"       audio: {vblanks} vblanks rendered ({k} at 60 Hz, {vblanks - k} at 50 Hz), crc {crc}")
    return ok


RENDER_LINE = re.compile(r"^\[input\] recording's renderer revision is (\d+), this exe's (\d+):")


def recording_render(res):
    """(recording's revision, this exe's) when the game said they differ,
    else None.  The game's own reading of `# render` (issue #60), not a
    second parser here: one that read a `# rendered ...` comment or missed
    an indented line as revision 0 quietly stopped the [frame] comparison.
    The game says it whenever the file names a revision or carries epochs,
    and refuses a malformed `# render` at boot (exit 13)."""
    for l in res.lines:
        m = RENDER_LINE.match(l)
        if m:
            return int(m.group(1)), int(m.group(2))
    return None


KEEP = None         # --keep-artifacts: where passing routes leave <route>.rec.pad / <route>.mcd
REPLAY_FROM = None  # --replay-from: another exe's --keep-artifacts directory


def prepare_keep_dir(keep, exe, logs, baseline):
    """Make `keep` an empty artifact directory, or say why not.  Emptied, not
    merged into: a route only writes its artifacts when it passes, so one left
    by an earlier (or another exe's) run would be replayed and byte-compared
    by --replay-from as if this run had made it - a stale PASS over a route
    that just failed.  Only artifacts (<route>.rec.pad, <route>.mcd) are
    removed, and never from a directory that is or holds --logs, the
    baseline, the repo, the exe or the current directory: this used to be
    an unconditional rmtree of whatever it was given (issue #61)."""
    cwd = Path.cwd().resolve()
    for what, other in (("--logs", Path(logs).resolve() if logs else None),
                        ("--compare-frames", baseline),
                        ("the repo", REPO.resolve()),
                        ("the current directory", cwd),
                        ("the exe's directory", exe.parent)):
        if other is not None and (keep == other or keep in other.parents):
            return f"--keep-artifacts {keep} is {what} or holds it - give it a directory of its own"
    if not keep.exists():
        keep.mkdir(parents=True)
        return None
    if not keep.is_dir():
        return f"--keep-artifacts {keep} is not a directory"
    entries = list(keep.iterdir())
    strays = [p.name for p in entries if not (p.name.endswith(".rec.pad") or p.suffix == ".mcd")]
    if strays:
        return (f"--keep-artifacts {keep} holds files that are not artifacts "
                f"({', '.join(strays[:5])}{', ...' if len(strays) > 5 else ''}) - give it an empty directory")
    for p in entries:
        p.unlink()
    return None


def first_difference(a, b):
    n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    return f"first difference at offset 0x{n:X} (sizes {len(a)} / {len(b)})"


def run_route_cross(exe, route, seed, logdir):
    """--replay-from: drive this exe with the recording ANOTHER exe made of
    the route (a 32-bit build's, for the x64 build: M9's A/B).  The recording
    carries that exe's display CRC every 300 vblanks, which the game checks
    itself ([replay] desync -> exit 13; RamUsed is skipped across ABIs,
    host/input.cpp); on top of that the [scene]/[frame] streams must equal
    the recording exe's log (--compare-frames, required with this option)
    and the memory card the run leaves must be byte-identical to the one the
    recording run left.  A recording from another renderer revision (the
    game's "[input] recording's renderer revision is" line) draws every frame
    differently on this exe by design, and the game skips the epochs' crc,
    so there only [scene] is held to the log; the card, the epochs'
    rng/ram and the route's checks still are."""
    rec = Path(REPLAY_FROM) / f"{route.name}.rec.pad"
    name = route.name + " (cross replay)"
    if not rec.exists():
        print(f"  FAIL {name}: no recording {rec}")
        return False
    args = ["--pad-file", str(rec), "--seed", str(seed),
            "--exit-after", str(route.exit_after)] + DETERMINISM + route.args
    log = Path(logdir) / f"{route.name}.log" if logdir else None
    print(f"== tier1 {name}: {' '.join(args)}"
          + (f"  env {route.env}" if route.env else ""))
    res = run_game(exe, args, route.env, route.timeout, log)
    ok = report_common(res, name)
    if res.scenes != route.expect:
        print(f"  FAIL {name}: [scene] sequence differs (expected vs actual):")
        for l in difflib.unified_diff(route.expect, res.scenes, "expected", "actual", lineterm="", n=2):
            print("       " + l)
        ok = False
    # The route's ceilings hold across ABIs too - except peak_ram, which is a
    # function of pointer size (x64 objects are bigger and the heap aligns to
    # 16).  peak_prim and peak_memnodes are exactly what would catch an
    # x64-only allocation regression, so they are not skipped with it.
    for k, ceiling in route.max.items():
        if k == "peak_ram":
            continue
        v = res.summary_int(k)
        if v is None:
            print(f"  FAIL {name}: no {k}= in [summary] to hold to its ceiling {ceiling}")
            ok = False
        elif v > ceiling:
            print(f"  FAIL {name}: {k}={v} exceeds {ceiling}")
            ok = False
    # The recording's `# render` against the exe's own, as the game read it.
    revs = recording_render(res)
    if revs:
        print(f"       renderer revision {revs[0]} -> {revs[1]}: "
              "[frame] lines not compared with the recording exe's")
    ok &= compare_baseline(res, name, f"{route.name}.log", scenes_only=revs is not None)
    frames = len(res.frame_crcs())
    if frames < route.min_frames:
        print(f"  FAIL {name}: {frames} distinct unmasked frame CRC(s), {route.min_frames} required")
        ok = False
    want = Path(REPLAY_FROM) / f"{route.name}.mcd"
    card = "no card on either side"
    if want.exists() != (res.card is not None):
        print(f"  FAIL {name}: memory card {'missing' if res.card is None else 'unexpected'} "
              f"(the recording run {'left' if want.exists() else 'did not leave'} one)")
        ok = False
    elif res.card is not None:
        ref = want.read_bytes()
        if ref != res.card:
            print(f"  FAIL {name}: card0.mcd differs from {want}: {first_difference(ref, res.card)}")
            ok = False
        card = f"card0.mcd {len(res.card)} bytes identical"
    epochs = sum(1 for l in rec.read_text(encoding="utf-8").splitlines() if l.startswith("# epoch"))
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {res.wall:.1f}s wall, "
          f"{res.summary.get('vblanks', '?')} vblanks, {epochs} epochs checked, "
          f"{frames} distinct frames, {card}, "
          f"peak_ram={res.summary.get('peak_ram', '?')} peak_prim={res.summary.get('peak_prim', '?')} "
          f"peak_memnodes={res.summary.get('peak_memnodes', '?')}")
    return ok


def run_route(exe, route, seed, logdir, replay):
    if REPLAY_FROM:
        return run_route_cross(exe, route, seed, logdir)
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
        if v is None:
            print(f"  FAIL {route.name}: no {k}= in [summary] to hold to its ceiling {ceiling}")
            ok = False
        elif v > ceiling:
            print(f"  FAIL {route.name}: {k}={v} exceeds {ceiling}")
            ok = False
    ok &= compare_baseline(res, route.name, f"{route.name}.log")
    frames = len(res.frame_crcs())
    if frames < route.min_frames:
        print(f"  FAIL {route.name}: {frames} distinct unmasked frame CRC(s), {route.min_frames} required")
        ok = False
    print(f"  {'PASS' if ok else 'FAIL'} {route.name}: {res.wall:.1f}s wall, "
          f"{res.summary.get('vblanks', '?')} vblanks, {len(res.scenes)} scenes, {frames} distinct frames, "
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
        if res2.card != res.card:
            print(f"  FAIL {route.name} (replay): card0.mcd differs from the recorded run's"
                  + (f": {first_difference(res.card, res2.card)}" if res.card and res2.card else ""))
            ok = False
        epochs = sum(1 for l in Path(rec).read_text(encoding="utf-8").splitlines() if l.startswith("# epoch"))
        print(f"  {'PASS' if ok else 'FAIL'} {route.name} (replay): {res2.wall:.1f}s wall, "
              f"{len(frames1)} frames compared, {epochs} epochs checked")
    if ok:
        if KEEP:
            shutil.copyfile(rec, Path(KEEP) / f"{route.name}.rec.pad")
            if res.card is not None:
                (Path(KEEP) / f"{route.name}.mcd").write_bytes(res.card)
        os.unlink(rec)
    else:
        print(f"       recording kept: {rec}")
    return ok


def tier1(exe, seed, fast, logdir, only, replay, territory):
    if fast:
        names = FAST_ROUTES + FAST_ROUTES_EXTRA.get(territory, [])
    else:
        names = sorted(p.stem for p in ROUTES.glob("*.pad") if p.stem != "walk_right")
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
        route = Route(path, territory)
        if route.territory and route.territory != territory:
            print(f"  skip {n}: {route.territory}-only route ({territory} build)")
            continue
        ok &= run_route(exe, route, seed, logdir, replay)
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
        good &= compare_baseline(res, name, f"tier2_{name}.log")
        crcs = res.frame_crcs(after_scene="Game")
        if len(crcs) < 2:
            print(f"  FAIL {name}: only {len(crcs)} distinct unmasked frame CRC(s) after [scene] Game")
            good = False
        late, ended = res.frame_crcs_after_open("Game", TIER2_LATE_START,
                                                TIER2_LATE_START + TIER2_LATE_WINDOW)	# see TIER2_LATE_FLOOR
        if len(late) < TIER2_LATE_FLOOR and not ended:
            print(f"  FAIL {name}: only {len(late)} distinct unmasked frame CRC(s) in vblanks "
                  f"{TIER2_LATE_START}-{TIER2_LATE_START + TIER2_LATE_WINDOW} after [scene] Game "
                  f"({TIER2_LATE_FLOOR} required) - the level stopped moving after its fade-in")
            good = False
        print(f"  {'PASS' if good else 'FAIL'} {name}: {res.wall:.1f}s wall, {len(crcs)} distinct frames "
              f"({len(late)} in vblanks {TIER2_LATE_START}-{TIER2_LATE_START + TIER2_LATE_WINDOW} after open"
              f"{', level ended inside the window' if ended else ''}), "
              f"peak_ram={res.summary.get('peak_ram', '?')} peak_prim={res.summary.get('peak_prim', '?')} "
              f"peak_memnodes={res.summary.get('peak_memnodes', '?')}")
        ok &= good
    return ok


def pe_image_base(path):
    """the preferred ImageBase in an exe's PE optional header (PE32 or PE32+)"""
    b = Path(path).read_bytes()[:4096]
    pe, = struct.unpack_from("<I", b, 0x3C)
    if b[:2] != b"MZ" or b[pe:pe + 4] != b"PE\0\0":
        return None
    opt = pe + 24
    magic, = struct.unpack_from("<H", b, opt)
    if magic == 0x10B:
        return struct.unpack_from("<I", b, opt + 28)[0]
    if magic == 0x20B:
        return struct.unpack_from("<Q", b, opt + 24)[0]
    return None


def check_link(res, exe):
    """the fault self-test's [crash] line: link must be the exe file's
    ImageBase + rva - the address addr2line wants, on x86 and x64 alike
    (issue #62).  Returns an error string, or None."""
    line = next((l for l in res.lines if l.startswith("[crash] code=")), None)
    m = re.search(r"\brva=0x([0-9A-Fa-f]+) link=(?:0x)?([0-9A-Fa-f]+)\b", line or "")
    if not m:
        return f"no rva/link in {line!r}"
    base = pe_image_base(exe)
    rva, link = int(m.group(1), 16), int(m.group(2), 16)
    if base is None or link != base + rva:
        return f"link 0x{link:X} != ImageBase {base if base is None else hex(base)} + rva 0x{rva:X}"
    return None


def selftest(exe, seed, logdir, territory="USA"):
    # (name, env, exit code, tag the log must show, line that makes it a SKIP)
    cases = [
        ("assert", {"SBSP_SELFTEST": "assert@100"}, 10, "[assert]", None),
        ("fault", {"SBSP_SELFTEST": "fault@100"}, 11, "[crash] code=0xC0000005", None),
        ("hang", {"SBSP_SELFTEST": "hang@100", "SBSP_WATCHDOG": "3"}, 12, "[watchdog]", None),
        ("assert-continue", {"SBSP_SELFTEST": "assert@100", "SBSP_ASSERT_CONTINUE": "1"}, 0, "[assert]", None),
        # the CRT terminations that raise no SEH exception (issue #62)
        ("abort", {"SBSP_SELFTEST": "abort@100"}, 11, "[crash] kind=abort", None),
        # a second abort() from a fault-safe exit hook: SIGABRT must still be
        # armed, or the CRT ends the process with 3 under a [summary] of 11
        ("abort-in-hook", {"SBSP_SELFTEST": "abort-in-hook@100"}, 11, "[crash] kind=abort", None),
        ("terminate", {"SBSP_SELFTEST": "terminate@100"}, 11, "[crash] kind=terminate", None),
        ("invalid-param", {"SBSP_SELFTEST": "invalid-param@100"}, 11, "[crash] kind=invalid-parameter",
         "[selftest] invalid-param returned"),
        ("stack-overflow", {"SBSP_SELFTEST": "stack-overflow@100"}, 11, "[crash] code=0xC00000FD", None),
    ]
    ok = True
    for name, env, want, tag, skip in cases:
        # --pad-script: nothing pressed, but a scripted-input run like every
        # other one here (no sbsp.ini, live keyboard and pad ignored)
        args = ["--level", "1-1", "--seed", str(seed), "--exit-after", "300",
                "--pad-script", "0:0000"] + DETERMINISM
        log = Path(logdir) / f"selftest_{name}.log" if logdir else None
        res = run_game(exe, args, env, 120, log)
        # msvcrt.dll (the MinGW exe's CRT) never calls the invalid-parameter
        # handler; the clang-cl exes' static UCRT must, so no SKIP there
        if skip and any(l.startswith(skip) for l in res.lines) and \
                re.search(rb"(?i)msvcrt\.dll\0", Path(exe).read_bytes()):
            print(f"  SKIP selftest {name}: msvcrt.dll does not report it (exit {res.code})")
            continue
        tagged = any(l.startswith(tag) for l in res.lines)
        # the exit code the process returned is the one [summary] states,
        # and there is exactly one [summary], whoever else called Port_Exit
        summaries = sum(1 for l in res.lines if l.startswith("[summary]"))
        summary = res.summary.get("exit") == str(want) and summaries == 1
        # the self-test provokes forbidden tags on purpose, so FORBIDDEN as a
        # whole does not apply - but it must read no sbsp.ini, like every run
        ini = [l for l in res.lines if l.startswith("[ini] loaded")]
        link = check_link(res, exe) if name == "fault" else None
        good = res.code == want and tagged and summary and not ini and not link
        if ini:
            print(f"       {ini[0]}")
        if link:
            print(f"       {link}")
        print(f"  {'PASS' if good else 'FAIL'} selftest {name}: exit {res.code} (want {want}), "
              f"{tag} {'seen' if tagged else 'MISSING'}, [summary] "
              f"{'exit=' + res.summary['exit'] if 'exit' in res.summary else 'MISSING'}"
              f"{'' if summaries == 1 else f' ({summaries} of them)'}"
              f"{', link = ImageBase + rva' if name == 'fault' and not link else ''}")
        ok &= good
    ok &= selftest_paced(exe, seed, logdir)
    ok &= selftest_wav(exe, seed, logdir, territory)
    return ok


def selftest_wav(exe, seed, logdir, territory):
    """--dump-audio leaves a valid WAV on every exit path (issue #62): the
    header is patched after each vblank's write and the clean/assert exits
    close it through an exit hook.  RIFF must equal the file size - 8, the
    data chunk must be a whole number of vblanks (44100/hz frames of s16
    stereo) and fit the file - exactly, on the clean and assert paths - and
    the PCM must not be silence.  The per-vblank sync alone already leaves
    the file exact, so the file cannot prove the hook ran: those two paths
    must also log "[host] audio dump closed: <frames>" matching the data
    chunk, and the fault and watchdog paths must not (the hook is not
    fault-safe, and the watchdog thread runs no main-thread hook)."""
    per_vblank = (44100 // (50 if territory == "EUR" else 60)) * 4
    cases = [
        ("clean", {}, 0, True),
        ("assert", {"SBSP_SELFTEST": "assert@200"}, 10, True),
        ("fault", {"SBSP_SELFTEST": "fault@200"}, 11, False),
        ("hang", {"SBSP_SELFTEST": "hang@200", "SBSP_WATCHDOG": "3"}, 12, False),
    ]   # (name, env, exit code, the exit hook closes it)
    ok = True
    tmp = tempfile.mkdtemp(prefix="sbsp_wav_")
    try:
        for name, env, want, hooked in cases:
            wav = Path(tmp) / f"{name}.wav"
            args = ["--level", "1-1", "--seed", str(seed), "--exit-after", "300",
                    "--pad-script", "0:0000", "--dump-audio", str(wav)] + DETERMINISM
            log = Path(logdir) / f"selftest_wav_{name}.log" if logdir else None
            res = run_game(exe, args, env, 120, log)
            b = wav.read_bytes() if wav.exists() else b""
            problems = []
            if res.code != want:
                problems.append(f"exit {res.code}, want {want}")
            if len(b) < 44 or b[0:4] != b"RIFF" or b[8:16] != b"WAVEfmt " or b[36:40] != b"data":
                problems.append(f"no WAV header ({len(b)} bytes)")
                riff = data = 0
            else:
                riff, = struct.unpack_from("<I", b, 4)
                data, = struct.unpack_from("<I", b, 40)
                if riff != len(b) - 8:
                    problems.append(f"RIFF size {riff} != file size - 8 ({len(b) - 8})")
                if data == 0 or data % per_vblank:
                    problems.append(f"data size {data} is not a whole number of {per_vblank}-byte vblanks")
                if data > len(b) - 44 or (hooked and data != len(b) - 44):
                    problems.append(f"data size {data} {'!=' if hooked else '>'} file size - 44 ({len(b) - 44})")
                if not any(b[44:44 + data]):
                    problems.append("PCM is all zero")
            closed = [l for l in res.lines if l.startswith("[host] audio dump closed:")]
            if hooked:
                m = re.match(r"\[host\] audio dump closed: (\d+) frames$", closed[-1] if closed else "")
                if not m:
                    problems.append("no '[host] audio dump closed' line - the exit hook did not run")
                elif int(m.group(1)) * 4 != data:
                    problems.append(f"hook closed {m.group(1)} frames, data chunk holds {data // 4}")
            elif closed:
                problems.append(f"the exit hook ran on this path: {closed[0]!r}")
            good = not problems
            print(f"  {'PASS' if good else 'FAIL'} selftest wav {name}: exit {res.code}, "
                  f"{len(b)} bytes, data {data} = {data / per_vblank:.2f} vblanks, "
                  f"hook {'closed it' if closed else 'did not run'}"
                  + ("" if good else " - " + "; ".join(problems)))
            ok &= good
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def selftest_paced(exe, seed, logdir):
    """Paced CD loads are emulated time (cd/cd.cpp, issue #67).  Everything
    else here runs --no-cd-pace so its baselines stay put; this is the one
    check that a paced load is deterministic too: two uncapped runs WITHOUT
    --no-cd-pace must agree frame for frame, and must open the level later
    than an instant-load run does, or pacing was never on."""
    paced = [a for a in DETERMINISM if a != "--no-cd-pace"]
    base = ["--level", "1-1", "--seed", str(seed), "--exit-after", "400", "--pad-script", "0:0000"]
    runs = []
    for name, extra in (("paced_1", paced), ("paced_2", paced), ("instant", DETERMINISM)):
        log = Path(logdir) / f"selftest_{name}.log" if logdir else None
        runs.append(run_game(exe, base + extra, {}, 120, log))

    def stream(res):
        return [l for l in res.lines if l.startswith("[frame] ") or l.startswith("[scene] ")]

    def first_open(res):
        opens = [int(l.split("vblank=")[1]) for l in res.lines if l.startswith("[scene] ") and "vblank=" in l]
        return opens[0] if opens else None

    ok = all([report_common(r, "selftest paced") for r in runs])   # a list: every run reports, not just up to the first failure
    s1, s2 = stream(runs[0]), stream(runs[1])
    same = bool(s1) and s1 == s2
    v_paced, v_instant = first_open(runs[0]), first_open(runs[2])
    later = v_paced is not None and v_instant is not None and v_paced > v_instant
    ok = ok and same and later
    print(f"  {'PASS' if ok else 'FAIL'} selftest paced loads: {len(s1)} [scene]/[frame] lines "
          f"{'identical' if same else 'DIFFER'} across two runs, level opens at vblank "
          f"{v_paced} paced vs {v_instant} instant")
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
    ap.add_argument("--territory", choices=list(TERRITORIES), default="USA",
                    help="the exe's territory build: selects '# territory' routes and "
                         "'# usa'/'# eur' header lines (default USA)")
    ap.add_argument("--logs", help="directory to keep every run's log")
    ap.add_argument("--compare-frames", metavar="DIR",
                    help="a --logs directory from another build of the same territory/variant: "
                         "every tier 1 / tier 2 run's [scene] + [frame] CRC stream must be identical to it")
    ap.add_argument("--keep-artifacts", metavar="DIR",
                    help="tier 1: keep each passing route's recording (<route>.rec.pad) and the "
                         "memory card it left (<route>.mcd) here, for another exe's --replay-from")
    ap.add_argument("--replay-from", metavar="DIR",
                    help="tier 1: instead of playing each route, replay the recording another exe "
                         "left in DIR (--keep-artifacts) and require no desync, the same streams "
                         "(needs --compare-frames with that exe's --logs) and the same memory card")
    a = ap.parse_args()

    exe = Path(a.exe).resolve()
    if not exe.exists():
        print(f"no such exe: {exe}")
        return 2
    if a.logs:
        Path(a.logs).mkdir(parents=True, exist_ok=True)
    if a.compare_frames:
        global BASELINE
        BASELINE = Path(a.compare_frames).resolve()
        if not BASELINE.is_dir():
            print(f"no such baseline directory: {BASELINE}")
            return 2
        # run_game writes <logs>/<name>.log BEFORE compare_baseline reads
        # <baseline>/<name>.log, so the same directory for both would have
        # every run compare against the log it just wrote itself - a green
        # sweep that proves nothing.  Keeping this run's logs is legitimate;
        # it just needs its own directory.
        if a.logs and Path(a.logs).resolve() == BASELINE:
            print(f"--logs and --compare-frames are the same directory ({BASELINE}): "
                  f"every run would be compared against itself - give --logs a different one")
            return 2
    if not (a.tier1 or a.tier2 or a.selftest):
        ap.error("nothing to do: pass --tier1, --tier2 and/or --selftest")
    global KEEP, REPLAY_FROM
    if a.keep_artifacts:
        if a.replay_from:
            ap.error("--keep-artifacts and --replay-from: a cross replay records nothing to keep")
        KEEP = Path(a.keep_artifacts).resolve()
        err = prepare_keep_dir(KEEP, exe, a.logs, BASELINE)
        if err:
            print(err)
            return 2
    if a.replay_from:
        # without the other exe's logs a cross replay would only prove "no
        # desync at the 300-vblank epochs" - the stream compare is the oracle
        if not a.compare_frames:
            ap.error("--replay-from needs --compare-frames (the recording exe's --logs directory)")
        REPLAY_FROM = Path(a.replay_from).resolve()
        if not REPLAY_FROM.is_dir():
            print(f"no such artifact directory: {REPLAY_FROM}")
            return 2

    only_routes = only_levels = None
    if a.only is not None:
        only_routes = [x for x in a.only if not x.isdigit()]
        only_levels = [int(x) for x in a.only if x.isdigit()]
        bad = [x for x in only_levels if not 0 <= x <= 24]
        if bad:
            ap.error(f"--only: level index out of range 0-24: {bad}")

    ok = True
    if a.selftest:
        ok &= selftest(exe, a.seed, a.logs, a.territory)
    if a.tier1:
        ok &= tier1(exe, a.seed, a.fast, a.logs, only_routes, not a.no_replay, a.territory)
    if a.tier2:
        ok &= tier2(exe, a.seed, a.short, a.logs, only_levels)
    print("ALL PASSED" if ok else "FAILURES")
    return 0 if ok else EXIT_ORACLE


if __name__ == "__main__":
    sys.exit(main())
