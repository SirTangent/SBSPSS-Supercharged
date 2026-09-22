#!/usr/bin/env python3
"""Pre-stretch the keyboard key caps for the PlayStation's non-square pixels.

The game draws into a 512x256 frame and the port scans it out into a 4:3
box (port/psyq/vk/viewport.cpp, and a CRT before that), so a pixel on
screen is 1.5 times taller than it is wide.  Climax's own art is drawn in
that space: the +but*.bmp pad glyphs are 18x11, which is how a round
circle button ends up round at 18*2/3 = 12 across by 11 down.

The key caps of github issue #43 came off a modern sprite sheet drawn for
square pixels, and went in at the size they were cut (14x14, 16x14 for the
wide ones).  That is right in the file and wrong on screen, where the caps
read as narrow upright slabs.  The screenshots they were reviewed from were
--dump-frames BMPs blown up 2x, i.e. square pixels, which is exactly why it
got through.

So the masters in port/art/keycaps/ stay as the artist cut them, square,
and this writes the Graphics/UI/+key*.bmp the data build consumes with
every column resampled to the width that renders as the master's shape:
the largest even width no wider than 1.5x the master (14 -> 20, 16 -> 24).

Even, because the resample is mirror-symmetric: with an even width the
duplicated columns pair up about the centre line and an up arrow stays
pointing straight up.  The closest even widths to the exact 1.5x sit
either side of it - 14x1.5 is 21, and 20 and 22 are equally wrong - so the
tie goes to the narrower, which doubles six of the fourteen columns rather
than eight and keeps the letter strokes nearer the master's weight.

Nearest-neighbour and nothing else: these are nine-colour pixel art on one
shared CLUT, and any filtering would invent colours the CLUT has no room
for.  Re-running is safe - it always reads the masters, never its output.

Usage:  python port/tools/stretch_keycaps.py [--check]

        --check  compare instead of write, and exit 1 on any difference
                 (so CI, or a reviewer, can tell the two are in step)
"""

import os
import struct
import sys

HERE      = os.path.dirname(os.path.abspath(__file__))
REPO      = os.path.normpath(os.path.join(HERE, "..", ".."))
MASTERS   = os.path.join(REPO, "port", "art", "keycaps")
OUT       = os.path.join(REPO, "Graphics", "UI")

#	512 across a 4:3 frame of 256 lines: a source pixel is 2/3 as wide as tall.
PIXEL_ASPECT_NUM = 3
PIXEL_ASPECT_DEN = 2


def stretched_width(w):
	"""The width that renders as w square pixels, rounded down to even."""
	return ((w * PIXEL_ASPECT_NUM) // PIXEL_ASPECT_DEN) & ~1


def column_map(src_w, dst_w):
	"""Mirror-symmetric nearest-neighbour column pick, dst_w even."""
	assert dst_w % 2 == 0, "an odd width cannot be mirror-symmetric"
	half = [min(src_w - 1, ((2 * x + 1) * src_w) // (2 * dst_w))
	        for x in range(dst_w // 2)]
	return half + [src_w - 1 - s for s in reversed(half)]


def read_bmp4(path):
	"""-> (w, h, header bytes up to the pixel data, rows of nibble indices).

	Top row first.  Only the 4bpp uncompressed bottom-up BMPs the art is
	in are understood; anything else is a mistake worth stopping for."""
	with open(path, "rb") as f:
		d = f.read()
	if d[:2] != b"BM":
		raise ValueError("%s: not a BMP" % path)
	off = struct.unpack("<I", d[10:14])[0]
	w, h, planes, bpp, comp = struct.unpack("<iiHHI", d[18:34])
	if bpp != 4 or comp != 0 or planes != 1:
		raise ValueError("%s: want an uncompressed 4bpp BMP, got %dbpp/comp %d"
		                 % (path, bpp, comp))
	if h < 0:
		raise ValueError("%s: top-down BMPs are not handled" % path)
	stride = ((w * 4 + 31) // 32) * 4
	rows = []
	for y in range(h):
		r = d[off + y * stride:off + y * stride + stride]
		rows.append([(r[x // 2] >> 4) if x % 2 == 0 else (r[x // 2] & 15)
		             for x in range(w)])
	rows.reverse()
	return w, h, d[:off], rows


def write_bmp4(header, w, h, rows):
	"""The master's header with the width, strides and sizes patched."""
	stride = ((w * 4 + 31) // 32) * 4
	pixels = bytearray()
	for row in reversed(rows):
		line = bytearray(stride)
		for x, idx in enumerate(row):
			if x % 2 == 0:
				line[x // 2] |= (idx & 15) << 4
			else:
				line[x // 2] |= (idx & 15)
		pixels += line
	out = bytearray(header)
	struct.pack_into("<I", out, 2, len(header) + len(pixels))	# bfSize
	struct.pack_into("<i", out, 18, w)							# biWidth
	struct.pack_into("<I", out, 34, len(pixels))				# biSizeImage
	return bytes(out + pixels)


def main(argv):
	check = "--check" in argv[1:]
	for arg in argv[1:]:
		if arg != "--check":
			sys.stderr.write("unknown argument %s\n" % arg)
			return 2

	names = sorted(n for n in os.listdir(MASTERS) if n.lower().endswith(".bmp"))
	if not names:
		sys.stderr.write("no masters in %s\n" % MASTERS)
		return 2

	stale = 0
	for name in names:
		src_w, h, header, rows = read_bmp4(os.path.join(MASTERS, name))
		dst_w = stretched_width(src_w)
		cols = column_map(src_w, dst_w)
		blob = write_bmp4(header, dst_w, h, [[r[c] for c in cols] for r in rows])

		dst = os.path.join(OUT, name)
		old = open(dst, "rb").read() if os.path.exists(dst) else None
		if old == blob:
			print("%-16s %2dx%-2d -> %2dx%-2d  up to date" % (name, src_w, h, dst_w, h))
			continue
		stale += 1
		if check:
			print("%-16s %2dx%-2d -> %2dx%-2d  STALE" % (name, src_w, h, dst_w, h))
			continue
		with open(dst, "wb") as f:
			f.write(blob)
		print("%-16s %2dx%-2d -> %2dx%-2d  written" % (name, src_w, h, dst_w, h))

	if check and stale:
		sys.stderr.write("%d cap(s) out of step with port/art/keycaps - "
		                 "run port/tools/stretch_keycaps.py\n" % stale)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
