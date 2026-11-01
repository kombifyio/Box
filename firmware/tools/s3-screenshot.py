"""Capture the round S3 Box screen (1.85C, 360x360) over its USB CDC port into a PNG.

Usage: python s3-screenshot.py COM4 out.png

Sends `KBX shot`; the firmware redraws once and streams "#SHOT <w> <h> rgb565",
one "#SHOT <y> <base64 row>" line per row and "#SHOT END" (usb_log.c). Only the
standard library and pyserial are needed. The RLCD frame is 1 bit: use
scripts/kbx-remote.ps1 -Screenshot there.
"""
import base64
import struct
import sys
import time
import zlib

import serial


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    port, out = sys.argv[1], sys.argv[2]
    s = serial.Serial(port, 115200, timeout=0.5)
    s.dtr = True   # the TinyUSB CDC mirrors its log only while the host asserts DTR
    s.reset_input_buffer()
    s.write(b"KBX shot\n")
    buf = b""
    deadline = time.time() + 40
    while time.time() < deadline and b"#SHOT END" not in buf:
        buf += s.read(65536)
    s.close()
    w = h = 0
    rows = {}
    for line in buf.split(b"\n"):
        if line.startswith(b"#SHOT END"):
            break
        parts = line.rstrip(b"\r").split(b" ", 3)
        if parts[0] != b"#SHOT":
            continue
        if len(parts) == 4 and parts[3] == b"rgb565":
            w, h = int(parts[1]), int(parts[2])
        elif len(parts) == 3 and parts[1].isdigit():
            try:
                rows[int(parts[1])] = base64.b64decode(parts[2], validate=True)
            except Exception:
                pass
    if not w or not rows:
        sys.exit("no frame received (is the CDC port the app's, and the box a round S3?)")
    rgb = bytearray(w * h * 3)
    for y, px in rows.items():
        for x in range(min(w, len(px) // 2)):
            v = px[2 * x] | (px[2 * x + 1] << 8)
            o = (y * w + x) * 3
            rgb[o] = ((v >> 11) & 0x1F) * 255 // 31
            rgb[o + 1] = ((v >> 5) & 0x3F) * 255 // 63
            rgb[o + 2] = (v & 0x1F) * 255 // 31
    png(out, w, h, bytes(rgb))
    print(f"{out}: {len(rows)}/{h} rows")


if __name__ == "__main__":
    main()
