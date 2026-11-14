"""port/tools/build_pc.py's command line (issue #98), on fixed cases.

build_pc.py replaced port/build-pc.sh with the same command line: issue #98
held it to what build-pc.sh did with 47 argument lists (its build/test/soak/
parity64 steps stubbed out), all equal.  Issue #39 then renamed the presets
(x64 the plain ones, x86-* the 32-bit ones, the clangcl* words refused), and
the table below is that grammar, so a change to it shows up here.  Also:
--no-data and the territories whose data a command line builds first;
game_sources.cmake must be what port/cmake/gen_game_sources.py makes from
makefile.gaz, and the PS1 build's object list must resolve.

    build_scripts_test.py
"""
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "psx"))
import build_pc  # noqa: E402
import buildenv  # noqa: E402
import psx_manifest  # noqa: E402

# (command line, (mode, presets, ninja args)) - None is a usage error.
# For parity64 the presets are (EUR infix, variant).
CASES = [
    ('', ('build', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('all', ('build', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('usa', ('build', ('debug', 'final'), ())),
    ('eur', ('build', ('eur-debug', 'eur-final'), ())),
    ('USA', ('build', ('debug', 'final'), ())),
    ('debug', ('build', ('debug',), ())),
    ('final', ('build', ('final',), ())),
    ('usa-debug', ('build', ('debug',), ())),
    ('usa-final', ('build', ('final',), ())),
    ('eur-debug', ('build', ('eur-debug',), ())),
    ('eur-final', ('build', ('eur-final',), ())),
    ('x86', ('build', ('x86-debug', 'x86-final'), ())),
    ('x86 usa', ('build', ('x86-debug', 'x86-final'), ())),
    ('x86 eur', ('build', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('x86 EUR', ('build', ('x86-eur-debug', 'x86-eur-final'), ())),
    # both territories for 32-bit, as `all` is for x64
    ('x86 all', ('build', ('x86-debug', 'x86-final', 'x86-eur-debug', 'x86-eur-final'), ())),
    ('x86 ALL -j4', ('build', ('x86-debug', 'x86-final', 'x86-eur-debug', 'x86-eur-final'), ('-j4',))),
    ('test x86 all', ('test', ('x86-debug', 'x86-final', 'x86-eur-debug', 'x86-eur-final'), ())),
    ('soak x86 all', ('soak', ('x86-debug', 'x86-final', 'x86-eur-debug', 'x86-eur-final'), ())),
    ('test usa all', None),
    ('test x86 all eur', None),
    ('x86-debug', ('build', ('x86-debug',), ())),
    ('x86-final', ('build', ('x86-final',), ())),
    ('x86-eur-debug', ('build', ('x86-eur-debug',), ())),
    ('x86-eur-final -j4', ('build', ('x86-eur-final',), ('-j4',))),
    ('x86 eur -j4', ('build', ('x86-eur-debug', 'x86-eur-final'), ('-j4',))),
    ('debug -j2 -v', ('build', ('debug',), ('-j2', '-v'))),
    ('Debug', ('build', ('debug',), ())),
    ('test', ('test', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('test usa', ('test', ('debug', 'final'), ())),
    ('test eur', ('test', ('eur-debug', 'eur-final'), ())),
    ('test x86', ('test', ('x86-debug', 'x86-final'), ())),
    ('test x86 eur', ('test', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('test x86-debug', ('test', ('x86-debug',), ())),
    ('test final', ('test', ('final',), ())),
    ('soak', ('soak', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('soak eur', ('soak', ('eur-debug', 'eur-final'), ())),
    ('soak x86 eur', ('soak', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('parity64', ('parity64', ('', 'debug'), ())),
    ('parity64 eur', ('parity64', ('eur-', 'debug'), ())),
    ('parity64 final', ('parity64', ('', 'final'), ())),
    ('parity64 eur final', ('parity64', ('eur-', 'final'), ())),
    ('parity64 FINAL EUR', ('parity64', ('eur-', 'final'), ())),
    ('parity64 debug usa', ('parity64', ('', 'debug'), ())),
    ('bogus', None),
    ('x86 xyz', ('build', ('x86-debug', 'x86-final'), ('xyz',))),
    ('test bogus', None),
    ('test x86 xyz', None),
    ('test usa eur extra', None),
    # a territory word only follows x86; after anything else it would be
    # dropped without a word (PR #100 / #101 reviews)
    ('test usa eur', None),
    ('test debug eur', None),
    ('soak eur usa', None),
    ('soak x y z', None),
    ('test x86 usa eur', None),
    ('parity64 eur final x', None),
    ('parity64 nope', None),
    ('clangcl', None),
    ('clangcl64 eur', None),
    ('clangcl-x64-debug', None),
    ('test clangcl64', None),
]

failed = 0
for line, want in CASES:
    try:
        mode, presets, extra = build_pc.plan(shlex.split(line))
        got = (mode, tuple(presets), tuple(extra))
    except build_pc.Usage:
        got = None
    ok = got == want
    failed += not ok
    print("%s  build-pc %s" % ("pass" if ok else "FAIL", line or "(no arguments)"))
    if not ok:
        print("      got:  %r" % (got,))
        print("      want: %r" % (want,))

# --no-data comes out of the command line wherever it stands, before plan()
# sees it (Ninja has no such option to lose), and SBSP_NO_DATA says the same
# from the environment, where a false spelling means "build the data" (PR
# #108 review).  (command line, variable, (what plan() gets, why skipped))
ARG = buildenv.NO_DATA_ARG


def from_env(value):
    return "%s=%s in the environment" % (buildenv.NO_DATA_ENV, value)


NO_DATA_CASES = [
    ('debug', None, ('debug', '')),
    ('--no-data debug', None, ('debug', ARG)),
    ('debug --no-data', None, ('debug', ARG)),
    ('debug -j2 --no-data -v', None, ('debug -j2 -v', ARG)),
    ('test --no-data x86 eur', None, ('test x86 eur', ARG)),
    ('parity64 eur final --no-data', None, ('parity64 eur final', ARG)),
    ('--no-data', None, ('', ARG)),
    ('debug --no-data --no-data', None, ('debug', ARG)),
    ('debug --nodata', None, ('debug --nodata', '')),      # Ninja's to refuse
    ('debug', '1', ('debug', from_env('1'))),
    ('debug', 'ON', ('debug', from_env('ON'))),
    ('debug', 'yes', ('debug', from_env('yes'))),
    ('debug', '0', ('debug', '')),
    ('debug', '', ('debug', '')),
    ('debug', 'OFF', ('debug', '')),
    ('debug', 'off', ('debug', '')),
    ('debug', 'false', ('debug', '')),
    ('debug', 'No', ('debug', '')),
    ('debug', 'n', ('debug', '')),
    ('debug', ' 0 ', ('debug', '')),
    ('debug --no-data', '0', ('debug', ARG)),
    ('debug --no-data', '1', ('debug', ARG)),
]
saved = os.environ.pop(buildenv.NO_DATA_ENV, None)
for line, var, want in NO_DATA_CASES:
    if var is None:
        os.environ.pop(buildenv.NO_DATA_ENV, None)
    else:
        os.environ[buildenv.NO_DATA_ENV] = var
    rest, why = buildenv.no_data(shlex.split(line))
    got = (" ".join(rest), why)
    ok = got == want
    failed += not ok
    print("%s  no_data %s%s" % ("pass" if ok else "FAIL", line,
                                "" if var is None else "  (%s=%r)" % (buildenv.NO_DATA_ENV, var)))
    if not ok:
        print("      got:  %r" % (got,))
        print("      want: %r" % (want,))
os.environ.pop(buildenv.NO_DATA_ENV, None)
if saved is not None:
    os.environ[buildenv.NO_DATA_ENV] = saved

# What a command line builds, in order: a territory's data once, ahead of
# its first tree and not before another territory's trees.
STEP_CASES = [
    ('', 'data:USA debug final data:EUR eur-debug eur-final'),
    ('all', 'data:USA debug final data:EUR eur-debug eur-final'),
    ('usa', 'data:USA debug final'),
    ('eur', 'data:EUR eur-debug eur-final'),
    ('final', 'data:USA final'),
    ('eur-debug', 'data:EUR eur-debug'),
    ('x86', 'data:USA x86-debug x86-final'),
    ('x86 eur', 'data:EUR x86-eur-debug x86-eur-final'),
    ('x86 all', 'data:USA x86-debug x86-final data:EUR x86-eur-debug x86-eur-final'),
    ('x86-eur-final', 'data:EUR x86-eur-final'),
    ('test', 'data:USA debug final data:EUR eur-debug eur-final'),
    ('test x86 eur', 'data:EUR x86-eur-debug x86-eur-final'),
    ('soak eur', 'data:EUR eur-debug eur-final'),
    ('parity64', 'data:USA x86-debug debug'),
    ('parity64 final', 'data:USA x86-final final'),
    ('parity64 eur', 'data:EUR x86-eur-debug eur-debug'),
    ('parity64 FINAL EUR', 'data:EUR x86-eur-final eur-final'),
]
for line, want in STEP_CASES:
    mode, presets, _ = build_pc.plan(shlex.split(line))
    got = " ".join("data:" + w if s == "data" else w for s, w in build_pc.build_steps(mode, presets))
    ok = got == want
    failed += not ok
    print("%s  steps of build-pc %s: %s" % ("pass" if ok else "FAIL", line or "(no arguments)", got))
    if not ok:
        print("      want: %r" % (want,))

# A Ninja dry run must not start a real data build.
for line, want in (('debug', False), ('debug -j4 -v', False), ('debug -- -n', True), ('debug -n', True),
                   ('debug -- --dry-run', True), ('x86 eur -- -j2 -n', True), ('debug -- -nv', False)):
    _, _, extra = build_pc.plan(shlex.split(line))
    ok = build_pc.dry_run(extra) == want
    failed += not ok
    print("%s  dry run: build-pc %s is %sone" % ("pass" if ok else "FAIL", line, "" if want else "not "))

# The reasons a build leaves the data alone without being asked, and the
# lock (buildenv; PR #108 review).
with tempfile.TemporaryDirectory() as tmp:
    # out\ or out\<T> as a junction to another checkout's: that data is not
    # this checkout's to rebuild
    def junction(link, target):
        return subprocess.run(["cmd", "/c", "mklink", "/J", link, target],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

    other = os.path.join(tmp, "other", "out")
    os.makedirs(os.path.join(other, "USA"))
    plain = os.path.join(tmp, "plain")
    os.makedirs(os.path.join(plain, "out", "USA"))
    whole = os.path.join(tmp, "whole")
    os.mkdir(whole)
    part = os.path.join(tmp, "part")
    os.makedirs(os.path.join(part, "out"))
    made = junction(os.path.join(whole, "out"), other) and junction(os.path.join(part, "out", "USA"),
                                                                    os.path.join(other, "USA"))
    for label, got, want in (
            ("junctions can be made here", made, True),
            ("an ordinary out is this checkout's", buildenv.shared_out(plain, "USA"), ""),
            ("no out at all is this checkout's", buildenv.shared_out(os.path.join(tmp, "none"), "USA"), ""),
            ("out as a junction is not", buildenv.shared_out(whole, "USA"), "out"),
            ("out/USA as a junction is not", buildenv.shared_out(part, "usa"), os.path.join("out", "USA")),
            ("out/EUR beside a linked out/USA is", buildenv.shared_out(part, "EUR"), "")):
        ok = got == want
        failed += not ok
        print("%s  shared out: %s" % ("pass" if ok else "FAIL", label))
        if not ok:
            print("      got %r, want %r" % (got, want))
    for name in ("whole/out", "part/out/USA"):          # a junction is removed as a directory, target untouched
        if os.path.lexists(os.path.join(tmp, name)):
            os.rmdir(os.path.join(tmp, name))

    # cache_value, which the data step reads SBSP_DATA_TOOLS and
    # SBSP_DATA_FINAL_DAT with
    with open(os.path.join(tmp, "CMakeCache.txt"), "w") as f:
        f.write("# comment\nSBSP_DATA_TOOLS:STRING=vintage\nSBSP_DATA_FINAL_DAT:FILEPATH=\n"
                "SBSP_OTHER:FILEPATH=C:/a=b/final.dat\nSBSP_DATA_TOOLS_X:STRING=no\n")
    for key, want in (("SBSP_DATA_TOOLS", "vintage"), ("SBSP_DATA_FINAL_DAT", ""),
                      ("SBSP_OTHER", "C:/a=b/final.dat"), ("SBSP_MISSING", None)):
        got = buildenv.cache_value(tmp, key)
        ok = got == want
        failed += not ok
        print("%s  cache_value %s%s" % ("pass" if ok else "FAIL", key, "" if ok else ": got %r" % (got,)))
    ok = buildenv.cache_value(os.path.join(tmp, "nowhere"), "SBSP_DATA_TOOLS") is None
    failed += not ok
    print("%s  cache_value of a tree that is not there" % ("pass" if ok else "FAIL"))

    # what the caller asked for comes back as the reason, whatever else holds
    ok = buildenv.data_left_alone("USA", "--no-data") == "--no-data"
    failed += not ok
    print("%s  data_left_alone: the caller's reason comes first" % ("pass" if ok else "FAIL"))

    # hold_lock: a byte another process holds is waited for; any other
    # error stops the build, where it used to wait for ever
    real_locking, real_sleep = buildenv.msvcrt.locking, buildenv.time.sleep
    real_stderr = sys.stderr
    calls = []

    def fake_locking(errors):
        def locking(fd, mode, nbytes):
            calls.append(mode)
            if len(calls) <= len(errors):
                raise OSError(errors[len(calls) - 1], "test")
        return locking

    buildenv.time.sleep = lambda s: None
    sys.stderr = open(os.devnull, "w")
    try:
        for label, errors, want in (
                ("free: taken at once", [], 1),
                ("held twice (EACCES), then free: waits", [buildenv.errno.EACCES] * 2, 3),
                ("held (EDEADLOCK), then free: waits", [buildenv.LOCK_HELD[1]], 2),
                ("an error that is not contention stops the build", [buildenv.errno.EINVAL], SystemExit),
                ("held, then an error: stops", [buildenv.errno.EACCES, buildenv.errno.EBADF], SystemExit)):
            del calls[:]
            buildenv.msvcrt.locking = fake_locking(errors)
            try:
                buildenv.hold_lock(os.path.join(tmp, "test.lock"), "test").close()
                got = len(calls)
            except SystemExit:
                got = SystemExit
            ok = got == want
            failed += not ok
            real_stderr.flush()
            print("%s  hold_lock: %s" % ("pass" if ok else "FAIL", label))
    finally:
        sys.stderr.close()
        sys.stderr = real_stderr
        buildenv.msvcrt.locking, buildenv.time.sleep = real_locking, real_sleep

    # and for real: a second process waits for the first to let go
    lock = os.path.join(tmp, "real.lock")
    held = buildenv.hold_lock(lock, "test")
    waiter = subprocess.Popen(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, sys.argv[1]); import buildenv; buildenv.hold_lock(sys.argv[2], 'test').close()",
         os.path.join(HERE, "..", "tools"), lock], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    said = waiter.stderr.readline().decode(errors="replace")       # its "Another ... holds" line
    still = waiter.poll() is None
    held.close()
    rc = waiter.wait(timeout=30)
    waiter.stderr.close()
    ok = "waiting" in said and still and rc == 0
    failed += not ok
    print("%s  hold_lock: a second process waits, then gets it" % ("pass" if ok else "FAIL"))
    if not ok:
        print("      said %r, still waiting %r, exit %r" % (said, still, rc))

r = subprocess.run([sys.executable, os.path.join(HERE, "..", "cmake", "gen_game_sources.py"), "--check"])
failed += r.returncode != 0
print("%s  game_sources.cmake matches makefile.gaz" % ("pass" if r.returncode == 0 else "FAIL"))

# The PS1 build's object list (port/tools/psx/psx_manifest.py): every list
# resolves, every source it names exists, and the generated info.mip entry
# is this configuration's.  The order itself was held to make's (all four
# configurations, link file byte-identical) when issue #98 wrote it.
root = os.path.abspath(os.path.join(HERE, "..", ".."))
for t in ("USA", "EUR"):
    for v in ("DEBUG", "FINAL"):
        try:
            asm, cpp = psx_manifest.manifest(t, v)
            info = "system/%s/%s/CD/info" % (v, t)
            missing = [s for s in asm if s != info and not os.path.isfile(os.path.join(root, "source", s + ".mip"))]
            missing += [s for s in cpp if not os.path.isfile(os.path.join(root, "source", s + ".cpp"))]
            ok = info in asm and not missing and len(set(asm + cpp)) == len(asm + cpp)
            why = "missing %s" % missing[:3] if missing else ""
        except SystemExit as e:
            ok, why, asm, cpp = False, str(e), [], []
        failed += not ok
        print("%s  psx_manifest %s %s: %d asm + %d cpp %s" % ("pass" if ok else "FAIL", t, v, len(asm), len(cpp), why))

# The data build's `datastep.py run <path dir> <work dir> -- <command>`
# (port/tools/data): the 40 MkActor steps run side by side only because
# each one's command starts in a working directory of its own, with nothing
# an earlier run left in it (PR #107 review).  The probe writes down where
# it ran, the first directory on its PATH and whether the old file was
# still there.
DATASTEP = os.path.join(HERE, "..", "tools", "data", "datastep.py")
PROBE = ("import os, sys; open(sys.argv[1], 'w').write('\\n'.join("
         "[os.getcwd(), os.environ['PATH'].split(os.pathsep)[0], str(os.path.exists('Actor.Pak'))]))")


def datastep_run(args):
    return subprocess.run([sys.executable, DATASTEP, "run"] + args,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode


def same_dir(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


with tempfile.TemporaryDirectory() as tmp:
    path_dir = os.path.join(tmp, "path dir")
    os.mkdir(path_dir)
    for label, work, stale in (("a working directory that does not exist yet", os.path.join(tmp, "new", "Gary"), False),
                               ("one with an Actor.Pak left in it", os.path.join(tmp, "old work"), True)):
        if stale:
            os.mkdir(work)
            open(os.path.join(work, "Actor.Pak"), "w").close()
        report = os.path.join(tmp, "report.txt")
        if os.path.exists(report):
            os.remove(report)
        rc = datastep_run([path_dir, work, "--", sys.executable, "-c", PROBE, report])
        got = open(report).read().split("\n") if os.path.exists(report) else []
        ok = rc == 0 and len(got) == 3 and same_dir(got[0], work) and same_dir(got[1], path_dir) and got[2] == "False"
        failed += not ok
        print("%s  datastep run: %s" % ("pass" if ok else "FAIL", label))
        if not ok:
            print("      exit %d, the probe saw: %r" % (rc, got))
    # the form before the working directory was an argument, and a command
    # that fails
    for label, args in (("one directory, not two, is refused", [path_dir, "--", sys.executable, "-c", "pass"]),
                        ("no command is refused", [path_dir, os.path.join(tmp, "w"), "--"]),
                        ("no -- is refused", [path_dir, os.path.join(tmp, "w"), sys.executable]),
                        ("the command's failure is the step's", [path_dir, os.path.join(tmp, "w"), "--",
                                                                 sys.executable, "-c", "import sys; sys.exit(3)"])):
        rc = datastep_run(args)
        ok = rc == 1
        failed += not ok
        print("%s  datastep run: %s%s" % ("pass" if ok else "FAIL", label, "" if ok else " (exit %d)" % rc))

print("%d failed" % failed)
sys.exit(1 if failed else 0)
