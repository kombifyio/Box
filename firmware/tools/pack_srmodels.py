#!/usr/bin/env python3
"""Pack ESP-SR model directories into esp-sr's srmodels.bin format.

Same layout as esp-sr's model/pack_model.py (read by srmodel_load), with two
differences that matter when the pack is embedded in the app image instead of
an mmapped partition:

- files are packed in sorted order, so the layout is reproducible;
- every file starts on a 16-byte boundary inside the pack. The loader takes
  explicit start offsets, so the padding is invisible to it, and WakeNet's
  index and weights are read with word and 128-bit SIMD loads.

wakeword.c additionally places the whole pack on a 16-byte boundary.

usage: pack_srmodels.py <model_dir>... -o <out.bin>
"""
import argparse
import os
import struct

NAME_LEN = 32
ALIGN = 16


def name_bytes(name):
    raw = name.encode("ascii")
    if len(raw) > NAME_LEN:
        raise SystemExit(f"name too long for srmodels: {name}")
    return raw + b"\0" * (NAME_LEN - len(raw))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("models", nargs="+", help="model directories (e.g. .../wn9_hiesp)")
    parser.add_argument("-o", "--out", required=True)
    args = parser.parse_args()

    models = []
    for path in args.models:
        files = sorted(f for f in os.listdir(path) if os.path.isfile(os.path.join(path, f)))
        if not files:
            raise SystemExit(f"empty model directory: {path}")
        blobs = []
        for f in files:
            with open(os.path.join(path, f), "rb") as fh:
                blobs.append((f, fh.read()))
        models.append((os.path.basename(os.path.normpath(path)), blobs))

    file_count = sum(len(blobs) for _, blobs in models)
    header_len = 4 + len(models) * (NAME_LEN + 4) + file_count * (NAME_LEN + 4 + 4)
    offset = (header_len + ALIGN - 1) // ALIGN * ALIGN

    header = struct.pack("<I", len(models))
    data = bytearray(b"\0" * (offset - header_len))
    for model_name, blobs in models:
        header += name_bytes(model_name) + struct.pack("<I", len(blobs))
        for file_name, blob in blobs:
            header += name_bytes(file_name) + struct.pack("<II", offset, len(blob))
            data += blob
            offset += len(blob)
            pad = (-offset) % ALIGN
            data += b"\0" * pad
            offset += pad
    assert len(header) == header_len

    with open(args.out, "wb") as fh:
        fh.write(header)
        fh.write(data)


if __name__ == "__main__":
    main()
