"""The committed port/tools/lznp.exe must be the encoder in port/tools/lznp-src.

Every actor pack goes through the committed tool in build-data, but nothing
rebuilt it from its sources or compared the two: an edit to lznp_encode.cpp
with a hand-rebuilt exe, or an LFS pointer standing in for the exe, would
only show as corrupt sprites in play (issue #61).  Both tools encode the same
inputs - three synthetic buffers plus the files given - and the outputs must
match byte for byte.

    lznp_exe_check.py <lznp built from lznp-src> <committed lznp.exe> [files...]
"""
import os
import shutil
import subprocess
import sys
import tempfile


def encode(tool, src, dst):
    r = subprocess.run([tool, "-Q", src, dst], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"FAIL: {tool} exited {r.returncode} on {src}: {r.stderr.strip()}")
    if not os.path.exists(dst):
        raise SystemExit(f"FAIL: {tool} exited 0 but wrote no {os.path.basename(dst)} - not the lznp tool")
    return open(dst, "rb").read()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    # absolute: CreateProcess does not resolve a relative tool path with
    # forward slashes against the working directory
    built, committed = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    files = sys.argv[3:]
    size = os.path.getsize(committed)
    if size < 4096:
        print(f"FAIL: {committed} is {size} bytes - an LFS pointer, not the tool (git lfs pull)")
        return 1

    inputs = {"zeros": bytes(65536),
              "repeats": bytes((i % 7 + ord("a")) for i in range(100000))}
    s, noise = 12345, bytearray()
    for _ in range(100000):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        noise.append((s >> 16) & 0xFF)
    inputs["noise"] = bytes(noise)
    for f in files:
        inputs[os.path.basename(f)] = open(f, "rb").read()

    tmp = tempfile.mkdtemp(prefix="sbsp_lznp_")
    ok = True
    try:
        for name, data in inputs.items():
            src = os.path.join(tmp, name + ".in")
            open(src, "wb").write(data)
            a = encode(built, src, os.path.join(tmp, name + ".built"))
            b = encode(committed, src, os.path.join(tmp, name + ".committed"))
            same = a == b and len(a) > 0
            print(f"{name:20s} in={len(data):7d} built={len(a):7d} committed={len(b):7d}  "
                  f"{'OK' if same else 'MISMATCH'}")
            ok = ok and same
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("lznp_exe_check: " + ("the committed lznp.exe matches its sources" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
