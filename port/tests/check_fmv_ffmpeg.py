#!/usr/bin/env python3
"""One-time ffmpeg tolerance cross-check for the M7 movie pipeline.

Compares raster RGB24 frames dumped by fmv_pipeline_test
(SBSP_FMV_DUMP_RAW=<dir>) against ffmpeg's decode of the same movies -
the oracle is NOT bit-exact (different IDCT/YUV rounding), so this
reports per-movie tolerance stats rather than gating CI.  The committed
CRC goldens are the regression contract; this script justifies them, and
exits 1 when the worst channel delta exceeds TOLERANCE - the bound
vlc3_test holds THQ frame 1 to against the same oracle - so a wrong IDCT
or YUV change cannot be committed with regenerated goldens on the strength
of a printed number (issue #61).  Run it before SBSP_WRITE_GOLDEN=1.

Usage (repo root):  py port/tests/check_fmv_ffmpeg.py <rawdir>
"""
import os
import struct
import subprocess
import sys

SEC = 2336
MOVIES = {"thq": "thq.str", "climax": "climax.str",
          "intro": "intro.str", "demo": "demo.str"}
W, H, N = 320, 240, 30
TOLERANCE = 16      # vlc3_test layer 2: maxD <= 16 against ffmpeg's RGB24


def wrap_cdxa(path):
    raw = open(path, "rb").read()
    nsec = len(raw) // SEC

    def sector2352(raw2336, idx):
        sync = b"\x00" + b"\xff" * 10 + b"\x00"
        mm, rest = divmod(150 + idx, 75 * 60)
        ss, ff = divmod(rest, 75)
        bcd = lambda v: ((v // 10) << 4) | (v % 10)
        return sync + bytes([bcd(mm), bcd(ss), bcd(ff), 2]) + raw2336

    payload = b"".join(sector2352(raw[i * SEC:(i + 1) * SEC], i)
                       for i in range(nsec))
    fmt = b"fmt " + struct.pack("<I", 16) + b"\x00" * 16
    dat = b"data" + struct.pack("<I", len(payload)) + payload
    riff = (b"RIFF" + struct.pack("<I", 4 + len(fmt) + len(dat)) + b"CDXA"
            + fmt + dat)
    out = os.path.join(os.environ["TEMP"], "fmv_check.xa")
    open(out, "wb").write(riff)
    return out


def main():
    rawdir = sys.argv[1] if len(sys.argv) > 1 else "."
    worst = 0
    for slug, fname in MOVIES.items():
        src = os.path.join("data", "CDData", fname)
        if not os.path.exists(src):
            print(slug, "source missing - skipped")
            continue
        xa = wrap_cdxa(src)
        out = os.path.join(os.environ["TEMP"], "fmv_check.rgb")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", xa,
                        "-frames:v", str(N), "-f", "rawvideo",
                        "-pix_fmt", "rgb24", out], check=True)
        ref = open(out, "rb").read()
        os.remove(xa)
        os.remove(out)
        assert len(ref) == W * H * 3 * N, (slug, len(ref))

        maxd = 0
        sumd = 0
        over2 = 0
        total = 0
        for f in range(N):
            p = os.path.join(rawdir, "%s_%03d.rgb" % (slug, f + 1))
            ours = open(p, "rb").read()
            assert len(ours) == W * H * 3, p
            base = f * W * H * 3
            for i in range(0, W * H * 3):
                d = ours[i] - ref[base + i]
                if d < 0:
                    d = -d
                if d > maxd:
                    maxd = d
                sumd += d
                if d > 2:
                    over2 += 1
                total += 1
        print("%s: %d frames, maxDelta %d, meanAbs %.4f, >2: %d of %d"
              % (slug, N, maxd, sumd / total, over2, total))
        worst = max(worst, maxd)
    print("worst maxDelta across movies: %d (tolerance %d)" % (worst, TOLERANCE))
    if worst > TOLERANCE:
        print("FAIL: further from ffmpeg than vlc3_test allows - do not regenerate "
              "the CRC goldens from this build")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
