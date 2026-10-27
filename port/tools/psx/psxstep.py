#!/usr/bin/env python3
"""One step of the PS1 build (issue #98): port/psx's CMake project runs every
PSY-Q tool through this, from the repository root, as makefile.gaz did.

    psxstep.py cc <src.cpp> <obj.o> <depfile> -- <ccpsx flags...>
        ccpsx -c <flags> <src> -o <tmp>/a.o; dmpsx -b; objbodge -> <obj>
    psxstep.py asm <src.mip> <obj.o> -- <asmpsx options...>
        asmpsx <options> <src>,<obj>
    psxstep.py infomip <out.mip> <VERSION> <TERRITORY>
        the INF_Version / INF_Territory / INF_FileSystem strings
    psxstep.py linkfile <out.ln> <objdir> <code objects...>
        the linker command file build/lnk.mak's LINKER_FILE_MACRO wrote
    psxstep.py link <ln> <cpe> <sym> <map> -- <slink flags...>
        slink <flags> @<ln>,<cpe>,<sym>,<map>; symmunge <sym>

Paths are relative to the repository root, as make had them: ccpsx's -I.
and sn.ini's tool directories resolve from there, and DEBUG bakes __FILE__
into the executable.  ccpsx finds sn.ini through SN_PATH.  Each compile gets its own
temporary directory for TEMP/TMPDIR, the dependency file the compiler
writes (DEPENDENCIES_OUTPUT) and the intermediate a.o, which make shared
between all of them; so Ninja can run them side by side.  The compiler's
dependencies are handed to Ninja as an absolute-path depfile.

The text files are written as tools/glecho.exe wrote them: one line per
item, CRLF, and & for a double quote.
"""
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
PSYQ = os.path.join(ROOT, "tools", "psyq")
TAB = "\t"


def tool_env(tmp=None):
    """The environment makefile.gaz gave the tools (build/globals.mak): the
    PSY-Q and tool directories first on the PATH, PSYQ_PATH as build-psx
    passed it, and the GNU search variables exported empty."""
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([os.path.join(PSYQ, "bin"), os.path.join(PSYQ, "bin", "egcs"),
                                   os.path.join(ROOT, "tools"), env.get("PATH", "")])
    # ccpsx reads its sn.ini (compiler_path, assembler_path) from the
    # directory SN_PATH names; that is the one in tools/psyq/bin/egcs
    env["SN_PATH"] = env["PSYQ_PATH"] = os.path.join(PSYQ, "bin", "egcs")
    # globals.mak exported every search variable SN's and GNU's tools read as
    # empty, so a machine's own (an SDK's C_INCLUDE_PATH, say) cannot reach
    # the 1999 compiler
    for v in ("COMPILER_PATH", "ASSEMBLER_PATH", "LIBRARY_PATH", "C_PLUS_INCLUDE_PATH",
              "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "GCC_EXEC_PREFIX",
              "LINKER_PATH"):
        env[v] = ""
    env.pop("DEPENDENCIES_OUTPUT", None)
    env.pop("SUNPRO_DEPENDENCIES", None)
    if tmp:
        env["TEMP"] = env["TMP"] = env["TMPDIR"] = tmp
        env["DEPENDENCIES_OUTPUT"] = os.path.join(tmp, "dep.tmp")
    return env


def run(cmd, env):
    # Windows finds an exe on the parent's PATH, not the child's: resolve it
    # against the tools' PATH first
    exe = shutil.which(cmd[0], path=env["PATH"])
    if not exe:
        sys.exit("psxstep: %s not found (tools/psyq/bin, tools)" % cmd[0])
    r = subprocess.run([exe] + cmd[1:], cwd=ROOT, env=env)
    if r.returncode != 0:
        sys.exit("psxstep: %s failed (exit %d)" % (cmd[0], r.returncode))


def rel(path):
    return os.path.relpath(os.path.abspath(path), ROOT).replace("\\", "/")


def write_lines(path, lines):
    """As glecho: each line, then CRLF; & for a double quote."""
    data = "".join(l.replace("&", '"') + "\r\n" for l in lines).encode("latin-1")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if os.path.exists(path) and open(path, "rb").read() == data:
        return                          # unchanged: keep the timestamp
    with open(path, "wb") as f:
        f.write(data)


def split(argv):
    i = argv.index("--") if "--" in argv else len(argv)
    return argv[:i], argv[i + 1:]


def depfile(dep_tmp, obj, out):
    """The compiler's make-style dependency list, as an absolute-path depfile
    for Ninja: the object first, then every file it read."""
    text = open(dep_tmp, errors="replace").read().replace("\\\r\n", " ").replace("\\\n", " ")
    files = text.split(":", 1)[1].split() if ":" in text else []
    def absolute(p):
        p = p if os.path.isabs(p) else os.path.join(ROOT, p)
        return os.path.normpath(p).replace("\\", "/").replace(" ", "\\ ")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", newline="\n") as f:
        f.write("%s: %s\n" % (absolute(obj), " \\\n  ".join(absolute(p) for p in files)))


def cmd_cc(argv):
    (src, obj, dep), flags = split(argv)
    # Short and relative, as make's TEMP (out/<T>/<V>/CD/temp) was: the 1999
    # assembler that ccpsx runs on its temporary files crashes on a long path.
    # One directory per object under the tree's temp/, named by a hash.
    objs_root, _, rest = rel(obj).partition("/objs/")
    tmp = "%s/temp/%s" % (objs_root, hashlib.sha1(rest.encode()).hexdigest()[:8])
    shutil.rmtree(os.path.join(ROOT, tmp), ignore_errors=True)
    os.makedirs(os.path.join(ROOT, tmp))
    a_o = tmp + "/a.o"
    env = tool_env(tmp)
    run(["ccpsx", "-c"] + flags + [rel(src), "-o", a_o], env)
    run(["dmpsx", "-b", a_o], env)
    run(["objbodge", a_o, rel(obj)], env)
    depfile(os.path.join(ROOT, tmp, "dep.tmp"), obj, dep)
    shutil.rmtree(os.path.join(ROOT, tmp), ignore_errors=True)


def cmd_asm(argv):
    (src, obj), opts = split(argv)
    os.makedirs(os.path.dirname(os.path.abspath(obj)), exist_ok=True)
    run(["asmpsx"] + opts + ["%s,%s" % (rel(src), rel(obj))], tool_env())


def cmd_infomip(argv):
    out, version, territory = argv
    write_lines(out, [TAB + "section\t\t.rdata",
                      TAB + "global\t\t\tINF_Version",
                      TAB + "global\t\t\tINF_Territory",
                      TAB + "global\t\t\tINF_FileSystem",
                      "INF_Version\tdb &%s&,0" % version,
                      "INF_Territory\tdb &%s&,0" % territory,
                      "INF_FileSystem\tdb &CD&,0"])


# build/lnk.mak and makefile.gaz's link values for the CD build
RAM_SIZE, STACK_SIZE, PROG_ORG = "2", "3000", "0x80010000"
LNK_FILE_SYS, LNK_DEV_KIT, LNK_OVL = "1", "1", ""
STARTUP_OBJS = ["BootObj\\snmain"]
EXTRA_OBJS = ["tools/vlc/lib/vlc_bit.o"]
LIBS = ["libc", "libapi", "libetc", "libspu", "libsnd", "libgpu", "libcard", "libpress",
        "libcd", "libtap", "libcomb", "libmcrd", "libgte", "libpad", "libds"]
ALL_LIBS = (["tools/psyq/lib/%s.lib" % l for l in LIBS] + ["tools/mod/lib/xmplay.lib"]
            + ["tools/psyq/lib/CMXBoot.lib"])


def cmd_linkfile(argv):
    out, objdir, objs = argv[0], argv[1].rstrip("/"), argv[2:]
    L = [";nostripfile=tools/psyq/lib/libsn.lib",
         "text\t\t\t\tgroup ", "extractors\t\t\tgroup ", "extradtors\t\t\tgroup ",
         "data\t\t\t\tgroup ", "sdata\t\t\t\tgroup ", "sbss\t\t\t\tgroup \tbss",
         "bss\t\t\t\tgroup\tbss ", "cached\t\t\t\tgroup\tcache(0x20000000) ", "", "",
         "memend\t\tgroup\tbss", "", "",
         "\tsection .text,text", "\tsection .cached,cached", "\tsection .sdata,sdata",
         "\tsection .ctors,text", "\tsection .dtors,text ", "\tsection .data,data",
         "\tsection .rdata,text", "\tsection .sbss,sbss", "\tsection .bss,bss",
         "\tsection .memend,memend", "\tsection .last,memend", "", "", "", "",
         "LNK_RamSize\t\tequ\t" + RAM_SIZE, "LNK_StackSize\t\tequ " + STACK_SIZE,
         "LNK_OrgAddress\t\tequ " + PROG_ORG, "LNK_FileSys\t\tequ " + LNK_FILE_SYS,
         "LNK_DevKit\t\t\tequ " + LNK_DEV_KIT, LNK_OVL + " ", "",
         TAB + "regs" + TAB + "pc=__SN_ENTRY_POINT", "; CD Start-Up Objects", ""]
    for f in STARTUP_OBJS:
        L += [TAB + 'startstats "%s" ' % f, TAB + "include tools/psyq/lib/%s.obj" % f, TAB + "endstats"]
    L += ["", "; Main Game object files"]
    for o in objs:
        L += [TAB + 'startstats "%s" ' % o.replace("/", "."),
              TAB + "include %s/%s.o " % (objdir, o), TAB + "endstats"]
    for f in EXTRA_OBJS:
        L += [TAB + 'startstats "%s" ' % f.replace("/", "."), TAB + "include %s " % f, TAB + "endstats"]
    L += ["", "; Libs"]
    for f in ALL_LIBS:
        L += [TAB + 'startstats "%s"' % f, TAB + "inclib " + f, TAB + "endstats"]
    L += [""]
    write_lines(out, L)


def cmd_link(argv):
    (ln, cpe, sym, map_), flags = split(argv)
    env = tool_env()
    for f in (cpe, sym, map_):
        if os.path.exists(f):
            os.remove(f)
    run(["slink"] + flags + ["@%s,%s,%s,%s" % (rel(ln), rel(cpe), rel(sym), rel(map_))], env)
    run(["symmunge", rel(sym)], env)


def main(argv):
    sys.stdout.reconfigure(line_buffering=True)
    cmds = {"cc": cmd_cc, "asm": cmd_asm, "infomip": cmd_infomip, "linkfile": cmd_linkfile, "link": cmd_link}
    if not argv or argv[0] not in cmds:
        sys.exit(__doc__)
    cmds[argv[0]](argv[1:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
