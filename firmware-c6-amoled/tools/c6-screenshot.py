"""Capture the C6 Box screen over USB-Serial/JTAG into a PNG.

Usage: python c6-screenshot.py COM7 out.png

Sends `KBX shot`, collects the `#SHOT x1 y1 x2 y2 <base64 rows|...>` bands
the firmware streams while it redraws once, and writes a 368x448 PNG. Only
the standard library and pyserial are needed.
"""
import base64
import struct
import sys
import time
import zlib

import serial

W, H = 368, 448


def png(path, rgb):
    raw = b"".join(b"\x00" + rgb[y * W * 3:(y + 1) * W * 3] for y in range(H))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    port, out = sys.argv[1], sys.argv[2]
    # DTR/RTS low: opening the USB-Serial/JTAG port must not reset the chip.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False
    s.open()
    s.reset_input_buffer()
    s.write(b"KBX shot\n")
    buf = b""
    deadline = time.time() + 20
    while time.time() < deadline and b"#SHOT END" not in buf:
        buf += s.read(65536)
    s.close()
    rgb = bytearray(W * H * 3)
    bands = 0
    for line in buf.split(b"\n"):
        if not line.startswith(b"#SHOT ") or line.startswith(b"#SHOT END"):
            continue
        parts = line.split(b" ", 5)
        x1, y1, x2, y2 = (int(p) for p in parts[1:5])
        w = x2 - x1 + 1
        px = b"".join(base64.b64decode(r) for r in parts[5].split(b"|") if r)
        for i in range(len(px) // 2):
            v = px[2 * i] | (px[2 * i + 1] << 8)
            x, y = x1 + i % w, y1 + i // w
            if 0 <= x < W and 0 <= y < H:
                o = (y * W + x) * 3
                rgb[o] = ((v >> 11) & 0x1F) * 255 // 31
                rgb[o + 1] = ((v >> 5) & 0x3F) * 255 // 63
                rgb[o + 2] = (v & 0x1F) * 255 // 31
        bands += 1
    if not bands:
        sys.exit("no screenshot data received")
    png(out, bytes(rgb))
    print(f"{out}: {bands} bands")


if __name__ == "__main__":
    main()
