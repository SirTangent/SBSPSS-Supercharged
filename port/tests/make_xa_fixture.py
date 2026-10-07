# Build the xa_test real-data fixture:
#   - find 4 consecutive (stride-32) audio sectors of one channel in Track1.Ixa
#   - save them raw (4 x 2336) as port/tests/xa_fixture_sectors.bin
#   - wrap them as a RIFF CDXA (2352-byte sectors) and decode with ffmpeg's
#     adpcm_xa into port/tests/xa_fixture_golden.pcm (s16le mono)
import struct, subprocess, sys, os

IXA = r"data\CDData\Track1.Ixa"
SEC = 2336

data = open(IXA, "rb").read()
nsec = len(data) // SEC
print("sectors:", nsec)

def subhdr(i):
    s = data[i*SEC : i*SEC+8]
    return s[0], s[1], s[2], s[3]   # file, chan, submode, coding

# find a channel run: 4 consecutive stride-32 audio sectors, coding 0x04
found = None
for chan in range(1, 7):
    i = chan
    while i + 3*32 < nsec:
        ok = True
        for k in range(4):
            f, c, sm, cd = subhdr(i + k*32)
            if not (f == 1 and c == chan and (sm & 0x04) and cd == 0x04):
                ok = False
                break
        if ok:
            found = (chan, i)
            break
        i += 32
    if found:
        break

chan, start = found
print("channel", chan, "start sector", start)
for k in range(4):
    print("  sector", start + k*32, "subheader", subhdr(start + k*32))

sectors = [data[(start + k*32)*SEC : (start + k*32 + 1)*SEC] for k in range(4)]

os.makedirs("port/tests", exist_ok=True)
with open("port/tests/xa_fixture_sectors.bin", "wb") as f:
    for s in sectors:
        f.write(s)

# RIFF CDXA: 2352-byte sectors = 12-byte sync + 3-byte MSF + mode + our 2336
def sector2352(raw2336, idx):
    sync = b"\x00" + b"\xff"*10 + b"\x00"
    mm, rest = divmod(150 + idx, 75*60)
    ss, ff = divmod(rest, 75)
    bcd = lambda v: ((v//10)<<4) | (v%10)
    hdr = bytes([bcd(mm), bcd(ss), bcd(ff), 2])
    return sync + hdr + raw2336

payload = b"".join(sector2352(s, i) for i, s in enumerate(sectors))
fmt = b"fmt " + struct.pack("<I", 16) + b"\x00"*16
dat = b"data" + struct.pack("<I", len(payload)) + payload
riff = b"RIFF" + struct.pack("<I", 4 + len(fmt) + len(dat)) + b"CDXA" + fmt + dat
xa_path = os.path.join(os.environ["TEMP"], "xa_fixture.xa")
open(xa_path, "wb").write(riff)

pcm_path = "port/tests/xa_fixture_golden.pcm"
# check=True and the size assert: a failed or partial decode must not leave a
# golden behind that xa_test then holds the decoder to (issue #61)
r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", xa_path,
                    "-f", "s16le", "-acodec", "pcm_s16le", pcm_path], check=True)
print("ffmpeg exit", r.returncode)
got = os.path.getsize(pcm_path)
print("golden pcm bytes:", got, "(expect", 4*4032*2, ")")
assert got == 4*4032*2, "ffmpeg decoded %d bytes; the fixture needs %d" % (got, 4*4032*2)
