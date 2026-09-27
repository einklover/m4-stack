#!/usr/bin/env python3
"""62x64 1-bpp icon_home.bmp. Palette 0 black, 1 white. Bit 1 = white."""

import struct
from pathlib import Path

W, H = 62, 64


def blank():
    return [[1 for _ in range(W)] for _ in range(H)]


def ink(bits, x, y):
    if 0 <= x < W and 0 <= y < H:
        bits[y][x] = 0


def line(bits, x0, y0, x1, y1, t=2):
    steps = max(abs(x1 - x0), abs(y1 - y0), 1)
    for i in range(steps + 1):
        x = x0 + (x1 - x0) * i // steps
        y = y0 + (y1 - y0) * i // steps
        for dy in range(-t, t + 1):
            for dx in range(-t, t + 1):
                if max(abs(dx), abs(dy)) <= t:
                    ink(bits, x + dx, y + dy)


def rect(bits, x, y, w, h, t=2):
    for i in range(t):
        line(bits, x + i, y + i, x + w - 1 - i, y + i, 0)
        line(bits, x + i, y + h - 1 - i, x + w - 1 - i, y + h - 1 - i, 0)
        line(bits, x + i, y + i, x + i, y + h - 1 - i, 0)
        line(bits, x + w - 1 - i, y + i, x + w - 1 - i, y + h - 1 - i, 0)


def write_bmp(path, bits):
    row_bytes = (W + 31) // 32 * 4
    image_size = row_bytes * H
    header = bytearray(62)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 2, 62 + image_size)
    struct.pack_into("<I", header, 10, 62)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<iiHHII", header, 18, W, H, 1, 1, 0, image_size)
    struct.pack_into("<I", header, 46, 2)
    header[54:58] = bytes((0, 0, 0, 0))
    header[58:62] = bytes((255, 255, 255, 0))
    body = bytearray()
    for y in range(H - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(W):
            if bits[y][x]:
                row[x >> 3] |= 0x80 >> (x & 7)
        body += row
    path.write_bytes(header + body)


def main():
    bits = blank()
    rect(bits, 4, 4, 54, 56, 2)
    # Two tiles and an L path between them.
    rect(bits, 10, 14, 16, 16, 2)
    rect(bits, 36, 34, 16, 16, 2)
    line(bits, 18, 30, 18, 42, 1)
    line(bits, 18, 42, 36, 42, 1)
    out = Path(__file__).resolve().parents[1] / "icon_home.bmp"
    write_bmp(out, bits)
    data = out.read_bytes()
    assert len(data) == 574, len(data)
    assert data[:2] == b"BM"
    print(out, len(data))


if __name__ == "__main__":
    main()
