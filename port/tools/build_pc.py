#!/usr/bin/env python3
"""Configure + build the PC port; port/build-pc.cmd runs this (issue #98,
which replaced port/build-pc.sh with it, same command line).

    build-pc [<preset>|usa|eur|x86 [usa|eur]|all] [extra ninja args...]
    build-pc test [usa|eur|x86 [usa|eur]|<preset>]  build, then ctest -L unit and -L playthrough on each tree
                     (playthrough skipped, with a note, for a tree configured SBSP_PLAYTHROUGH=OFF)
    build-pc soak [usa|eur|x86 [usa|eur]|<preset>]  build, then the full Tier 1 + Tier 2 sweep on each tree
    build-pc parity64 [usa|eur] [debug|final]  build x86 + x64, then the x64 A/B (streams, cross replay, cards)

Every preset is clang-cl (port/CMakePresets.json, cmake/clangcl-toolchain.cmake;
issue #39 retired MinGW).  debug, final (USA; usa-debug / usa-final are
accepted aliases), eur-debug and eur-final (EUR, PAL 50Hz) are x64, the
exes that ship; `usa` / `eur` name a territory's pair and `all` (the
default) is all four.  x86-debug, x86-final, x86-eur-debug and x86-eur-final
build the same code for 32-bit, which parity64 holds x64 to; `x86 [usa|eur]`
names a pair.  The words of the clangcl-* presets those replace are refused
with their new names.

Each tree needs its territory's data first (one build serves DEBUG and
FINAL, issue #35):
    port\\build-data.cmd usa            (out/USA/include + out/USA/cd)
    port\\build-data.cmd eur            (out/EUR/include + out/EUR/cd)
build_one checks for both and names the missing command.  A tree that was
configured for another toolchain or architecture (the MinGW trees that used
the names debug, final, eur-debug and eur-final) is reconfigured --fresh.

CMake, ctest and Ninja come from port/tools/buildenv.py: the PATH, else
Visual Studio's CMake component.  The playthrough tests run under the
Python running this script (Python3_EXECUTABLE).

SBSP_PLAYTHROUGH=OFF in the environment configures every tree with
-DSBSP_PLAYTHROUGH=OFF (no playthrough tests registered).  It is only
passed when set, so a tree keeps what it was last configured with;
SBSP_PLAYTHROUGH=ON turns them back on.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)            # buildenv, also under python -I
import buildenv                     # noqa: E402

PORT = os.path.dirname(HERE)
REPO = os.path.dirname(PORT)

USAGE = ("usage: port\\build-pc.cmd [debug|final|usa-debug|usa-final|eur-debug|eur-final|"
         "x86-debug|x86-final|x86-eur-debug|x86-eur-final|usa|eur|x86 [usa|eur]|all|"
         "test [usa|eur|x86 [usa|eur]|<preset>]|"
         "soak [usa|eur|x86 [usa|eur]|<preset>]|"
         "parity64 [usa|eur] [debug|final]] [ninja args]")

SINGLE = ["debug", "final", "eur-debug", "eur-final",
          "x86-debug", "x86-final", "x86-eur-debug", "x86-eur-final"]

# the clang-cl words before issue #39 made clang-cl the only toolchain
RENAMED = {"clangcl": "x86", "clangcl64": "usa / eur",
           "clangcl-debug": "x86-debug", "clangcl-final": "x86-final",
           "clangcl-eur-debug": "x86-eur-debug", "clangcl-eur-final": "x86-eur-final",
           "clangcl-x64-debug": "debug", "clangcl-x64-final": "final",
           "clangcl-x64-eur-debug": "eur-debug", "clangcl-x64-eur-final": "eur-final"}

# the false spellings CMake accepts for a BOOL, as test_one reads the cache
CMAKE_FALSE = ("0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND")


class Usage(Exception):
    pass


def presets_for(word, territory=None):
    """The preset list a word (or `all`) stands for; `territory` is the word
    that may follow `x86` (USA when None)."""
    w = (word or "").lower()
    t = (territory or "usa").lower()
    if t not in ("usa", "eur"):
        raise Usage()
    e = "eur-" if t == "eur" else ""
    if w in RENAMED:
        raise Usage("'%s' is '%s' now: issue #39 made clang-cl the only toolchain, "
                    "x64 the plain presets and x86 the x86-* ones" % (word, RENAMED[w]))
    if w == "usa":
        return ["debug", "final"]
    if w == "eur":
        return ["eur-debug", "eur-final"]
    if w == "x86":
        return ["x86-%sdebug" % e, "x86-%sfinal" % e]
    if w in ("all", ""):
        return ["debug", "final", "eur-debug", "eur-final"]
    if w in SINGLE:
        return [w]
    if w == "usa-debug":            # the build-data spelling, same tree
        return ["debug"]
    if w == "usa-final":
        return ["final"]
    raise Usage()


def territory_of(preset):
    """The territory a preset builds, for its data and run_tier's --territory."""
    return "EUR" if preset.startswith("eur-") or "-eur-" in preset else "USA"


def arch_of(preset):
    return "x86" if preset.startswith("x86-") else "x64"


def cache_value(tree, key):
    """A CMakeCache.txt entry's value (any type), or None."""
    try:
        with open(os.path.join(tree, "CMakeCache.txt"), errors="replace") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


class Builder:
    def __init__(self):
        self.cmake = buildenv.find_cmake()
        self.ctest = buildenv.ctest_for(self.cmake)
        self.ninja = buildenv.find_ninja()

    def env(self, preset, **extra):
        env = dict(os.environ)
        env.update(extra)
        return env

    def stale(self, preset):
        """Why build/<preset> must be configured --fresh, or None: its cache
        is another toolchain's or architecture's (the MinGW trees had the
        names debug, final, eur-debug and eur-final until issue #39)."""
        tree = os.path.join(PORT, "build", preset)
        if not os.path.isfile(os.path.join(tree, "CMakeCache.txt")):
            return None
        toolchain = cache_value(tree, "CMAKE_TOOLCHAIN_FILE") or ""
        if not toolchain.endswith("clangcl-toolchain.cmake"):
            return "configured with %s" % (os.path.basename(toolchain) or "no toolchain file")
        arch = cache_value(tree, "SBSP_ARCH")
        if arch != arch_of(preset):
            return "configured for %s" % arch
        return None

    def run(self, cmd, preset, cwd=PORT, **extra):
        return subprocess.run(cmd, cwd=cwd, env=self.env(preset, **extra)).returncode

    def check_data(self, preset):
        # CMake only gates on the generated headers; out/<T>/cd is what
        # cd.cpp opens at run time (the same files for DEBUG and FINAL)
        terr = territory_of(preset)
        for f in ("out/%s/include/BigLump.h" % terr, "out/%s/cd/BIGLUMP.BIN" % terr):
            if not os.path.isfile(os.path.join(REPO, f)):
                sys.exit("missing %s - run: port\\build-data.cmd %s" % (f, terr.lower()))

    def build_one(self, preset, ninja_args=()):
        self.check_data(preset)
        print("=== configure+build: %s ===" % preset, flush=True)
        configure = [self.cmake, "--preset", preset,
                     "-DCMAKE_MAKE_PROGRAM=" + self.ninja.replace("\\", "/"),
                     "-DPython3_EXECUTABLE=" + sys.executable.replace("\\", "/")]
        why = self.stale(preset)
        if why:
            print("build/%s was %s: configuring it --fresh as clang-cl %s" % (preset, why, arch_of(preset)),
                  flush=True)
            configure.append("--fresh")
        if os.environ.get("SBSP_PLAYTHROUGH"):
            configure.append("-DSBSP_PLAYTHROUGH=" + os.environ["SBSP_PLAYTHROUGH"])
        if self.run(configure, preset) or \
           self.run([self.cmake, "--build", "--preset", preset] + list(ninja_args), preset):
            sys.exit(1)

    def test_one(self, preset):
        tree = os.path.join(PORT, "build", preset)
        # SBSP_TEST_STRICT: this tree owns its data (build-data staged the
        # movies; the fixtures and the music are committed), so a test that
        # finds an input missing and would otherwise exit 77 (ctest: skipped)
        # fails instead - a skip here is a lost input (tests/test_skip.h, #61)
        print("=== ctest (%s): unit ===" % preset, flush=True)
        if self.run([self.ctest, "--test-dir", tree, "--output-on-failure", "-L", "unit", "--no-tests=error"],
                    preset, SBSP_TEST_STRICT="1"):
            sys.exit(1)
        # --no-tests=error turns "the label matched nothing" into a failure.
        # A shim-only tree (-DSBSP_BUILD_GAME=OFF) has no playthrough tests,
        # though, and neither does one configured SBSP_PLAYTHROUGH=OFF (read
        # from the cache, which keeps it across build_one's re-configure).
        if not os.path.isfile(os.path.join(tree, "sbsp.exe")):
            print("=== ctest (%s): playthrough SKIPPED - no sbsp.exe (SBSP_BUILD_GAME=OFF?) ===" % preset)
            return
        pt = ""
        try:
            with open(os.path.join(tree, "CMakeCache.txt"), errors="replace") as f:
                for line in f:
                    if line.startswith("SBSP_PLAYTHROUGH:BOOL="):
                        pt = line.split("=", 1)[1].strip().upper()
        except OSError:
            pass
        if pt in CMAKE_FALSE or pt.endswith("-NOTFOUND"):
            print("=== ctest (%s): playthrough SKIPPED - opted out with SBSP_PLAYTHROUGH=OFF "
                  "(SBSP_PLAYTHROUGH=ON to re-enable) ===" % preset)
            return
        print("=== ctest (%s): playthrough ===" % preset, flush=True)
        if self.run([self.ctest, "--test-dir", tree, "--output-on-failure", "-L", "playthrough", "--no-tests=error"],
                    preset, SBSP_TEST_STRICT="1"):
            sys.exit(1)

    def tier(self, preset, *args):
        return self.run([sys.executable, os.path.join("tests", "run_tier.py"),
                         "--territory", territory_of(preset)] + list(args), preset)

    def soak_one(self, preset):
        print("=== soak (%s): tier 1 + tier 2, all routes / all levels ===" % preset, flush=True)
        if self.tier(preset, "--exe", "build/%s/sbsp.exe" % preset, "--selftest", "--tier1", "--tier2",
                     "--logs", "build/%s/soak-logs" % preset):
            sys.exit(1)

    def parity64(self, e, v):
        """The x64 A/B (M9): the x86 exe (x86-<v>) plays every Tier 1 route
        and Tier 2 level and keeps its logs, recordings and memory cards; the
        x64 exe (<v>) must then (1) produce the same [scene]/[frame] streams
        playing the same tiers itself and (2) replay the 32-bit recordings
        without a desync, to the same streams and byte-identical cards.

        Every leg runs even if an earlier one fails, and the combined result
        is the exit status: leg 3 is the only thing that exercises the
        cross-exe replay and the card compare, so a divergence in leg 2 (USA
        FINAL had one until conv_pc.md entry 69) must not be what stops it
        from ever being reached.  A USA A/B keeps its old directory,
        build/parity64-<v>; EUR adds the EUR-only routes (play_trailer)."""
        x86, x64 = "x86-%s%s" % (e, v), "%s%s" % (e, v)
        terr = territory_of(x86)
        d = "build/parity64-%s%s" % (e, v)
        rc = 0
        shutil.rmtree(os.path.join(PORT, d), ignore_errors=True)
        print("=== parity64 (%s %s): x86 baseline ===" % (terr, v), flush=True)
        # --no-replay: the baseline's own determinism replay is already a
        # ctest, and the recording leg 3 needs is written by the first pass
        rc |= self.tier(x86, "--exe", "build/%s/sbsp.exe" % x86, "--tier1", "--tier2",
                        "--no-replay", "--logs", d + "/L32", "--keep-artifacts", d + "/A32") != 0
        print("=== parity64 (%s %s): x64, same tiers ===" % (terr, v), flush=True)
        rc |= self.tier(x64, "--exe", "build/%s/sbsp.exe" % x64, "--tier1", "--tier2",
                        "--logs", d + "/L64", "--compare-frames", d + "/L32") != 0
        print("=== parity64 (%s %s): x64 replays the x86 recordings ===" % (terr, v), flush=True)
        rc |= self.tier(x64, "--exe", "build/%s/sbsp.exe" % x64, "--tier1",
                        "--logs", d + "/L64x", "--compare-frames", d + "/L32", "--replay-from", d + "/A32") != 0
        return rc


def plan(argv):
    """argv -> (mode, presets, extra): mode is build/test/soak/parity64;
    for parity64 `presets` is (infix, variant).  Raises Usage."""
    what = argv[0] if argv else "all"
    rest = argv[1:]
    lw = what.lower()
    if lw in ("test", "soak"):
        if len(rest) > 2:
            raise Usage()
        return lw, presets_for(rest[0] if rest else "all", rest[1] if len(rest) > 1 else None), []
    if lw == "parity64":
        # [usa|eur] [debug|final]: USA and DEBUG by default
        if len(rest) > 2:
            raise Usage()
        t, v = "usa", "debug"
        for w in rest:
            w = w.lower()
            if w in ("usa", "eur"):
                t = w
            elif w in ("debug", "final"):
                v = w
            else:
                raise Usage()
        return "parity64", ("eur-" if t == "eur" else "", v), []
    # `x86` may be followed by a territory word; what comes after that is
    # ninja's
    terr = None
    if lw == "x86" and rest and rest[0].lower() in ("usa", "eur"):
        terr, rest = rest[0], rest[1:]
    return "build", presets_for(what, terr), rest


def main(argv):
    sys.stdout.reconfigure(line_buffering=True)
    try:
        mode, presets, extra = plan(argv)
    except Usage as e:
        if e.args:
            print("build-pc: %s" % e.args[0], file=sys.stderr)
        print(USAGE, file=sys.stderr)
        return 1
    b = Builder()
    if mode == "parity64":
        e, v = presets
        b.build_one("x86-%s%s" % (e, v))
        b.build_one("%s%s" % (e, v))
        if b.parity64(e, v):
            return 1
    else:
        for p in presets:
            b.build_one(p, extra)
        if mode == "test":
            for p in presets:
                b.test_one(p)
        elif mode == "soak":
            for p in presets:
                b.soak_one(p)
    print("PC build complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
