#!/usr/bin/env python3
"""Golden vectors for mdec_test (M7).

Independent transcription of the psx-spx "MDEC Decompression" pseudocode
(real_idct_core + yuv_to_rgb), mirroring the two documented-as-unknown
rounding choices made in port/psyq/mdec/mdec.cpp (scaletable >> 3,
(sum + 0xFFF) >> 13, 16.16 truncating YUV) so a mismatch means a
transcription typo in the C side, not a design difference.  The IDCT
output is saturated like the MDEC's (9-bit sign extension, then -128..127;
issue #60), which psx-spx's pseudocode omits.

Run from the repo root:  py port/tests/make_mdec_golden.py
Writes port/tests/mdec_golden.bin (committed).
"""
import struct

SCALETABLE = [
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707, 0xB8E3, 0x9592, 0x8275,
    0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641,
    0x6A6D, 0xE707, 0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592,
    0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D, 0x5A82,
    0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3,
    0x30FB, 0x89BE, 0x7641, 0xCF04, 0xCF04, 0x7641, 0x89BE, 0x30FB,
    0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707,
]
SCALETABLE = [v - 0x10000 if v >= 0x8000 else v for v in SCALETABLE]


def sar(v, n):
    return v >> n  # python >> is arithmetic (floor) for ints, matching C >> on negatives


def idct(blk):
    src = list(blk)
    for _ in range(2):
        dst = [0] * 64
        for x in range(8):
            for y in range(8):
                s = 0
                for z in range(8):
                    s += src[y + z * 8] * sar(SCALETABLE[x + z * 8], 3)
                dst[x + y * 8] = sar(s + 0xFFF, 13)
        src = dst
    return [clamp(sign9(v), -128, 127) for v in src]


def sign9(v):
    v &= 0x1FF
    return v - 0x200 if v & 0x100 else v


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def yuv_to_rgb24(cr, cb, yblk, xx, yy, rgb):
    for y in range(8):
        for x in range(8):
            c_r = cr[((x + xx) >> 1) + ((y + yy) >> 1) * 8]
            c_b = cb[((x + xx) >> 1) + ((y + yy) >> 1) * 8]
            rc = sar(91881 * c_r, 16)
            gc = sar(-22525 * c_b - 46816 * c_r, 16)
            bc = sar(116130 * c_b, 16)
            Y = yblk[x + y * 8]
            o = ((y + yy) * 16 + (x + xx)) * 3
            rgb[o + 0] = clamp(Y + rc, -128, 127) + 128
            rgb[o + 1] = clamp(Y + gc, -128, 127) + 128
            rgb[o + 2] = clamp(Y + bc, -128, 127) + 128


def pseudo_blocks():
    """Deterministic pseudo data - mdec_test.cpp builds the same blocks."""
    cr = [((i * 7) % 61) - 30 for i in range(64)]
    cb = [((i * 13) % 91) - 45 for i in range(64)]
    ys = [[((i * 5 + q * 17) % 255) - 128 for i in range(64)] for q in range(4)]
    return cr, cb, ys


def main():
    out = bytearray(b"MDG1")

    # 1) IDCT of DC-only block (blk[0]=64)
    blk = [0] * 64
    blk[0] = 64
    out += struct.pack("<64h", *idct(blk))

    # 2) IDCT of a single AC impulse (raster index 9 = 100)
    blk = [0] * 64
    blk[9] = 100
    out += struct.pack("<64h", *idct(blk))

    # 3) Full macroblock yuv_to_rgb (no IDCT - blocks are already pixel domain)
    cr, cb, ys = pseudo_blocks()
    rgb = [0] * (16 * 16 * 3)
    for q, (xx, yy) in enumerate([(0, 0), (8, 0), (0, 8), (8, 8)]):
        yuv_to_rgb24(cr, cb, ys[q], xx, yy, rgb)
    out += bytes(rgb)

    # 4) IDCT saturation: large DC + AC terms whose raw outputs land in
    #    every regime of the 9-bit sign extension followed by the -128..127
    #    clamp: in range (unchanged); 128..255 (clamped to 127); 256..383
    #    (wrap to -256..-129, clamped to -128); 384..511 (wrap to -128..-1,
    #    passed unclamped); 512 and up (wrap back positive: 551 -> 39);
    #    -256..-129 (clamped to -128); -384..-257 (wrap to 128..255,
    #    clamped to 127); -512..-385 (wrap to 0..127, passed unclamped)
    blk = [0] * 64
    for i, v in {0: 300, 1: 1023, 8: 1023, 9: 1023, 10: -1023}.items():
        blk[i] = v
    out += struct.pack("<64h", *idct(blk))

    with open("port/tests/mdec_golden.bin", "wb") as f:
        f.write(out)
    print("wrote port/tests/mdec_golden.bin (%d bytes)" % len(out))


if __name__ == "__main__":
    main()
