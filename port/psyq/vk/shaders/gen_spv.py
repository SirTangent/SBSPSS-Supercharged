#!/usr/bin/env python3
"""Regenerate the embedded SPIR-V headers from the GLSL sources.

    python port/psyq/vk/shaders/gen_spv.py            write present.{vert,frag}.spv.h
    python port/psyq/vk/shaders/gen_spv.py --check    exit 1 if either is out of date

Needs glslc, which the Windows Vulkan SDK ships (SPIR-V is
architecture-independent, so any glslc works): %GLSLC%, else glslc on the
PATH, else %VULKAN_SDK%\\Bin\\glslc.exe, else the SDK the headers were made
with.  The .spv.h files are committed; run this only after editing a shader.
Issue #98 ported it from gen_spv.sh: one line per 16 bytes, four
little-endian 32-bit words, as `od -An -tx4` printed them.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PINNED = r"C:\VulkanSDK\1.4.350.0\Bin\glslc.exe"


def glslc():
    for exe in (os.environ.get("GLSLC"), shutil.which("glslc"),
                os.path.join(os.environ["VULKAN_SDK"], "Bin", "glslc.exe") if os.environ.get("VULKAN_SDK") else None,
                PINNED):
        if exe and os.path.isfile(exe):
            return exe
    sys.exit("no glslc: install the Vulkan SDK (https://vulkan.lunarg.com) or set GLSLC")


def header(stage, spv):
    words = [int.from_bytes(spv[i:i + 4], "little") for i in range(0, len(spv), 4)]
    lines = ["/* SPIR-V for present.%s - regenerate with port/psyq/vk/shaders/gen_spv.py */" % stage,
             "static const unsigned int g_present_%s_spv[] = {" % stage]
    for i in range(0, len(words), 4):
        lines.append("".join("0x%08x," % w for w in words[i:i + 4]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv):
    check = "--check" in argv
    exe = glslc()
    stale = 0
    for stage in ("vert", "frag"):
        src = os.path.join(HERE, "present." + stage)
        spv_path = src + ".spv"
        subprocess.run([exe, src, "-o", spv_path], check=True)
        with open(spv_path, "rb") as f:
            text = header(stage, f.read())
        out = src + ".spv.h"
        try:
            with open(out, newline="") as f:
                current = f.read().replace("\r\n", "\n")
        except OSError:
            current = None
        if check:
            if current != text:
                print("%s is out of date" % out, file=sys.stderr)
                stale = 1
            continue
        if current != text:
            with open(out, "w", newline="\n") as f:
                f.write(text)
        print("wrote present.%s.spv.h" % stage)
    return stale


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
