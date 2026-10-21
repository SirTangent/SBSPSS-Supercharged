#!/usr/bin/env python3
"""The data build's compound steps (issue #95).

port/data/CMakeLists.txt runs each converter directly where one command
does; these are the steps that need more than that.  Each one fails loudly
where the 1999 makefile's shell lines could not.

  gfx <Tga2Gfx> <lznp> <in.tga> <out.gfx>
      Tga2Gfx into a temp file beside the output, lznp -Q that to the
      output, delete the temp.  makefile.gfx used one a.tmp in the repo root
      for all 43 screens, so two could not run at once.

  parkgrab <written .h> <header> -- <parkgrab> <args...>
      parkgrab exits 0 whether or not it wrote anything (issue #42).
      Fail, and delete the bank, unless it wrote its .h, then move that .h
      to the include directory as makefile.gfx's mv did.

  run <dir> -- <command...>
      The command with <dir> first on its PATH: MkActor packs every frame
      through system("lznp.exe ...") and finds it there.

  stage <source> <destination>
      Copy a CD file the PC exe reads into out/<T>/cd, as build-data.sh's
      stage() did: refuse a missing file, or one under 1 MiB (an
      unmaterialised Git LFS pointer, a truncated movie), and copy only when
      the destination is missing or older.
"""
import os
import shutil
import subprocess
import sys


class StepError(Exception):
    pass


def call(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        raise StepError("%s exited %d" % (os.path.basename(cmd[0]), r.returncode))


def split_command(argv):
    if "--" not in argv:
        raise StepError("expected -- before the command")
    i = argv.index("--")
    return argv[:i], argv[i + 1:]


def gfx(tga2gfx, lznp, src, dst):
    tmp = dst + ".tmp"
    try:
        call([tga2gfx, src, tmp])
        call([lznp, "-Q", tmp, dst], stdout=subprocess.DEVNULL)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def parkgrab(written, header, cmd):
    if os.path.exists(written):
        os.remove(written)
    call(cmd)
    if not os.path.exists(written):
        for a in cmd:
            if a.startswith("-o:") and os.path.exists(a[3:]):
                os.remove(a[3:])
        raise StepError("%s exited 0 but wrote no %s (issue #42)" % (os.path.basename(cmd[0]), written))
    os.replace(written, header)


def run(path_dir, cmd):
    env = dict(os.environ)
    env["PATH"] = os.path.abspath(path_dir) + os.pathsep + env.get("PATH", "")
    call(cmd, env=env)


def stage(src, dst):
    if not os.path.isfile(src) or os.path.getsize(src) < 1048576:
        raise StepError("%s is missing, truncated, or an unmaterialised Git LFS pointer"
                        " (for data/CDData run: git lfs pull)" % src)
    if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
        shutil.copyfile(src, dst)
        print("Staged %s" % dst)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    step, args = argv[0], argv[1:]
    try:
        if step == "gfx" and len(args) == 4:
            gfx(*args)
        elif step == "parkgrab":
            files, cmd = split_command(args)
            if len(files) != 2 or not cmd:
                raise StepError("usage: parkgrab <written .h> <header> -- <parkgrab> <args...>")
            parkgrab(files[0], files[1], cmd)
        elif step == "run":
            dirs, cmd = split_command(args)
            if len(dirs) != 1 or not cmd:
                raise StepError("usage: run <dir> -- <command...>")
            run(dirs[0], cmd)
        elif step == "stage" and len(args) == 2:
            stage(*args)
        else:
            print(__doc__)
            return 2
    except (StepError, OSError) as e:
        print("datastep %s: %s" % (step, e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
