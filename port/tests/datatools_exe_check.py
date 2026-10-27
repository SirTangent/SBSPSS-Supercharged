"""The committed port/tools/x64 converters must be what Utils/ builds.

build-data runs the committed x64 converters (issue #48),
but nothing else rebuilds them from Utils/ or compares the two: an
edit to the sources with a hand-rebuilt exe, or an LFS pointer standing in for
one, would only show as a changed BigLump.Bin.  This tree's copies - built by
whichever preset configured it, x86 included - and the committed
ones convert the same synthetic inputs, and every output must match byte for
byte.  A difference that depends on the compiler (the sizeof(std::vector) MkData
once counted into a bank's size) shows up as a mismatch here too.

    datatools_exe_check.py <built MkData> <committed MkData.exe>
                           <built Tga2Gfx> <committed Tga2Gfx.exe>
                           <built scripter> <committed scripter.exe>
                           [--mkactor <built> <committed>] [--mklevel <built> <committed>]
                           [--parkgrab <built> <committed>] [--transtext <built> <committed>]

MkActor and MkLevel (clang-cl builds only) convert a small actor and the
smallest level from the repo, and parkgrab (clang-cl only too) packs the
shop and party sprites as makefile.gfx does, all run from the repo root.
transtext (clang-cl only) converts a synthetic final.dat.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def noise(n, seed):
    s, out = seed, bytearray()
    for _ in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((s >> 16) & 0xFF)
    return bytes(out)


def tga(width, height, depth, seed):
    """An uncompressed true-colour TGA: Tga2Gfx reads the 18-byte header and
    then 3 bytes a pixel, bottom row first, whatever the depth says."""
    hdr = struct.pack("<BBB2s2sBhhhhBB", 0, 0, 2, b"\0\0", b"\0\0", 0,
                      0, 0, width, height, depth, 0)
    return hdr + noise(width * height * 3, seed)


# MkData's script: lines end in CRLF (it skips two bytes at each CR), a
# `{ NAME align` line opens a bank, `}` closes it, `#` is a comment and a
# leading `-` lists a file outside the current bank.
SCRIPT_A = (b"{ CACHE_A 4\r\n"
            b"\tfirst.bin\r\n"
            b"\tsecond.dat\r\n"
            b"}\r\n"
            b"# a comment line\r\n"
            b"third.bin\r\n"
            b"{ CACHE_B 2048\r\n"
            b"\t-fourth.bin\r\n"
            b"\tfirst.bin\r\n"
            b"\tfifth.raw\r\n"
            b"}\r\n")
SCRIPT_B = (b"sixth.bin\r\n"
            b"{ EMPTY 4\r\n"
            b"}\r\n")
FILES = {"first.bin": 3001, "second.dat": 1, "third.bin": 4096,
         "fourth.bin": 777, "fifth.raw": 20000, "sixth.bin": 2049}

# scripter: an #include and #defines, the one global and the tmp locals,
# arithmetic, every statement form and two built-in calls.  The game's own
# scripts include the generated trans.scr, which a test job does not have.
SCR_DEFS = b"// test defs\r\n#define\tCHAR_X\t3\r\n#define TEXT_A\t\t41\r\n"
SCR_MAIN = (b"#include\tdefs.scr\r\n#define\tTWELVE\t12\r\n\r\n"
            b"$tmp0=TWELVE*3+4/2-1;\r\n$lives=$lives+1;\r\n"
            b"if($tmp0==37)\r\n{\r\n\t_setText(CHAR_X,TEXT_A);\r\n\tprint($tmp0);\r\n}\r\n"
            b"else\r\n{\r\n\t_setAmmoCount(2,$tmp1);\r\n}\r\n"
            b"$tmp2=0;\r\nwhile($tmp2<5)\r\n{\r\n\t$tmp2=$tmp2+1;\r\n\tpause;\r\n}\r\n"
            b"do\r\n{\r\n\tpause;\r\n}\r\nwhile(_getResponse()!=0);\r\n"
            b"if($tmp9>TWELVE)\r\n\t$tmp9=$tmp9-TWELVE;\r\nstop;\r\n")


# transtext: lang.pl's final.dat (CRLF; `[ID]` then `<language>=<text>`
# lines): a language left out takes the English, `\n` becomes a newline,
# printf conversions must match the English, bytes from 0x80 pass through,
# and an ID with no lines is dropped.
TRANS_FINAL = (b"[STR__FIRST]\r\neng=Level %d-%d\r\nger=Stufe %d-%d\r\n\r\n"
               b"[STR__SECOND]\r\neng=  two\\nlines \r\nfre=deux\\nlignes\r\nFRE=Deux\r\n\r\n"
               b"[STR__EMPTY]\r\n\r\n"
               b"[STR__THIRD]\r\neng=\x91caf\xe9\x92 sure\r\nswe=s ; comment\r\n"
               b"spa=tres\r\nita=tre\r\ndut=drie\r\njap=san\r\n\r\n")


def run(cmd, cwd, wants):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"FAIL: {os.path.basename(cmd[0])} exited {r.returncode}: "
                         f"{(r.stdout + r.stderr).strip()[-300:]}")
    out = []
    for w in wants:
        p = os.path.join(cwd, w)
        if not os.path.exists(p):
            raise SystemExit(f"FAIL: {os.path.basename(cmd[0])} exited 0 but wrote no {w}")
        out.append(open(p, "rb").read())
        os.remove(p)
    return out


def run_dir(cmd, cwd, outdir, env=None):
    """Run a tool that writes into outdir; return {file name: bytes} and empty the dir."""
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace", env=env)
    if r.returncode != 0:
        raise SystemExit(f"FAIL: {os.path.basename(cmd[0])} exited {r.returncode}: "
                         f"{(r.stdout + r.stderr).strip()[-300:]}")
    out = {}
    for f in sorted(os.listdir(outdir)):
        p = os.path.join(outdir, f)
        out[f] = open(p, "rb").read()
        os.remove(p)
    if not out:
        raise SystemExit(f"FAIL: {os.path.basename(cmd[0])} exited 0 but wrote nothing")
    return out


def main():
    argv = sys.argv[1:]
    # MkActor, MkLevel and parkgrab build with clang-cl only: optional pairs
    # after the six
    pairs = {}
    for opt in ("--mkactor", "--mklevel", "--parkgrab", "--transtext"):
        if opt in argv:
            i = argv.index(opt)
            pairs[opt] = [os.path.abspath(a) for a in argv[i + 1:i + 3]]
            del argv[i:i + 3]
    if len(argv) != 6 or any(len(p) != 2 for p in pairs.values()):
        print(__doc__)
        return 2
    # absolute: CreateProcess does not resolve a relative tool path with
    # forward slashes against the working directory
    (mk_built, mk_committed, tg_built, tg_committed,
     sc_built, sc_committed) = (os.path.abspath(a) for a in argv)
    for exe in [mk_committed, tg_committed, sc_committed] + [p[1] for p in pairs.values()]:
        size = os.path.getsize(exe)
        if size < 4096:
            print(f"FAIL: {exe} is {size} bytes - an LFS pointer, not the tool (git lfs pull)")
            return 1

    tmp = tempfile.mkdtemp(prefix="sbsp_datatools_")
    ok = True

    def check(name, a, b):
        nonlocal ok
        same = a == b and len(a) > 0
        print(f"{name:28s} built={len(a):8d} committed={len(b):8d}  {'OK' if same else 'MISMATCH'}")
        ok = ok and same

    try:
        # Tga2Gfx <in.tga> <out>: odd sizes, one pixel, the depth-24 shift
        # and its no-shift branch, and the real legal screen
        tgas = {"odd_37x23_d24": tga(37, 23, 24, 1), "one_pixel_d24": tga(1, 1, 24, 2),
                "noshift_64x8_d16": tga(64, 8, 16, 3)}
        legal = "graphics/screens/legal.tga"
        if os.path.exists(legal):
            tgas["legal.tga"] = open(legal, "rb").read()
        for name, data in tgas.items():
            src = os.path.join(tmp, "in.tga")
            open(src, "wb").write(data)
            (a,) = run([tg_built, "in.tga", "out.gfx"], tmp, ["out.gfx"])
            (b,) = run([tg_committed, "in.tga", "out.gfx"], tmp, ["out.gfx"])
            check("Tga2Gfx " + name, a, b)

        # MkData -b:<base> -s:<script>.. -o:<lump> -i:<header> [-a:<align>]
        base = os.path.join(tmp, "base")
        os.mkdir(base)
        for i, (name, size) in enumerate(FILES.items()):
            open(os.path.join(base, name.upper()), "wb").write(noise(size, 100 + i))
        open(os.path.join(tmp, "a.scr"), "wb").write(SCRIPT_A)
        open(os.path.join(tmp, "b.scr"), "wb").write(SCRIPT_B)
        for label, extra in (("default align", []), ("-a:512", ["-a:512"])):
            args = ["-b:base", "-s:a.scr", "-s:b.scr", "-o:LUMP.BIN", "-i:lump.h"] + extra
            a = run([mk_built] + args, tmp, ["LUMP.BIN", "lump.h"])
            b = run([mk_committed] + args, tmp, ["LUMP.BIN", "lump.h"])
            check(f"MkData lump ({label})", a[0], b[0])
            check(f"MkData header ({label})", a[1], b[1])

        # scripter <in.scr> <out.dat>
        open(os.path.join(tmp, "defs.scr"), "wb").write(SCR_DEFS)
        open(os.path.join(tmp, "t.scr"), "wb").write(SCR_MAIN)
        (a,) = run([sc_built, "t.scr", "t.dat"], tmp, ["t.dat"])
        (b,) = run([sc_committed, "t.scr", "t.dat"], tmp, ["t.dat"])
        check("scripter t.scr", a, b)

        root = os.getcwd()          # the repo root (the test's working directory)
        # MkActor: one small actor from the repo.  It packs through
        # system("lznp ..."), so the committed x64 lznp goes first on the
        # PATH, and it leaves Actor.Tmp / Actor.Pak in its working directory.
        if "--mkactor" in pairs:
            outdir = os.path.join(tmp, "actor")
            os.mkdir(outdir)
            env = dict(os.environ)
            env["PATH"] = os.path.join(root, "port", "tools", "x64") + os.pathsep + env.get("PATH", "")
            args = ["-r:" + os.path.join(root, "graphics", "Characters"), "-s:Render/Psx",
                    "-o:" + outdir, "-i:" + outdir, "Anenome", "-c:16"]
            a = run_dir([pairs["--mkactor"][0]] + args, tmp, outdir, env)
            b = run_dir([pairs["--mkactor"][1]] + args, tmp, outdir, env)
            for f in sorted(set(a) | set(b)):
                check("MkActor " + f, a.get(f, b""), b.get(f, b""))
        # MkLevel: the smallest level, from the repo root (MkLevel.ini's
        # BackGfxDir is relative to it), with makefile.gfx's options
        if "--mklevel" in pairs:
            outdir = os.path.join(tmp, "level")
            os.mkdir(outdir)
            args = ["-c:tools/data/bin", "graphics/levels/FMA/CONTROLROOM/CONTROLROOM.mex",
                    "-o:" + os.path.join(outdir, "FMA_CONTROLROOM.lvl"), "-i:" + outdir,
                    "-t:8,3,1", "-s:16", "-l", "-q"]
            a = run_dir([pairs["--mklevel"][0]] + args, root, outdir)
            b = run_dir([pairs["--mklevel"][1]] + args, root, outdir)
            for f in sorted(set(a) | set(b)):
                check("MkLevel " + f, a.get(f, b""), b.get(f, b""))
        # parkgrab: makefile.gfx's shop and party lines (the .spr, the .h
        # beside it, the -l: picture of VRAM, party's -k: report), then party
        # with compressed texture pages, and -x: (no VRAM placement)
        if "--parkgrab" in pairs:
            outdir = os.path.join(tmp, "sprites")
            os.mkdir(outdir)
            shop = [f"graphics/shop/{n}.bmp" for n in
                    "blower cake cupcake jelly2 partyhat prezzie sarnie teddy smalltoken".split()]
            party = sorted("graphics/party/" + f for f in os.listdir("graphics/party")
                           if f.lower().endswith(".bmp"))
            o = outdir.replace("\\", "/")
            runs = {"shop": ["-z+"] + shop + ["-b+", "-t:8,1,1", f"-l:{o}/shop.lbm", f"-o:{o}/shop.spr"],
                    "party": ["-c-", "-z+", "-r+", "-q+"] + party +
                             ["-b+", "-t:8,4,1", f"-l:{o}/party.lbm", f"-o:{o}/party.spr", f"-k:{o}/rep.txt"],
                    "party -y+": ["-z+"] + party + ["-b+", "-y+", "-t:8,4,1", f"-o:{o}/party.spr"],
                    "shop -x:": ["-z+"] + shop + [f"-x:{o}/shop.sprf"]}
            for label, args in runs.items():
                a = run_dir([pairs["--parkgrab"][0]] + args, root, outdir)
                b = run_dir([pairs["--parkgrab"][1]] + args, root, outdir)
                for f in sorted(set(a) | set(b)):
                    check(f"parkgrab {label} {f}", a.get(f, b""), b.get(f, b""))
        # transtext <final.dat> -h:<header> -o:<dir> [-a+]: every language's
        # .dat, id.dat and the header
        if "--transtext" in pairs:
            work = os.path.join(tmp, "text")
            outdir = os.path.join(work, "o")
            os.makedirs(outdir)
            # -a+ names language 1 `ame`, not `fre`
            american = TRANS_FINAL.replace(b"fre=", b"ame=").replace(b"FRE=", b"AME=")
            for label, extra, text in (("", [], TRANS_FINAL), (" -a+", ["-a+"], american)):
                open(os.path.join(work, "final.dat"), "wb").write(text)
                args = ["-h:o/trans.h", "-o:o"] + extra + ["final.dat"]
                a = run_dir([pairs["--transtext"][0]] + args, work, outdir)
                b = run_dir([pairs["--transtext"][1]] + args, work, outdir)
                for f in sorted(set(a) | set(b)):
                    check(f"transtext{label} {f}", a.get(f, b""), b.get(f, b""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("datatools_exe_check: " + ("the committed x64 converters match their sources" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
