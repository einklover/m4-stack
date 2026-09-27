"""62x64 1-bit icon_home.bmp. Palette matches m4-lua-huarong-plugin: 0 black, 1 white."""

import struct
from pathlib import Path

W, H = 62, 64


def blank():
    return [[0 for _ in range(W)] for _ in range(H)]


def rect(bits, x, y, w, h, black):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if 0 <= xx < W and 0 <= yy < H:
                bits[yy][xx] = 1 if black else 0


def write_bmp(path, black):
    row_bytes = (W + 31) // 32 * 4
    image_size = row_bytes * H
    data_offset = 62
    header = bytearray(data_offset)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 2, data_offset + image_size)
    struct.pack_into("<I", header, 10, data_offset)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<iiHHII", header, 18, W, H, 1, 1, 0, image_size)
    struct.pack_into("<I", header, 46, 2)
    header[54:58] = bytes((0, 0, 0, 0))
    header[58:62] = bytes((255, 255, 255, 0))
    body = bytearray()
    for y in range(H - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(W):
            if not black[y][x]:
                row[x >> 3] |= 0x80 >> (x & 7)
        body += row
    path.write_bytes(header + body)


def main():
    bits = blank()
    rect(bits, 8, 10, 46, 44, True)
    rect(bits, 14, 16, 34, 32, False)
    rect(bits, 18, 20, 26, 24, True)
    rect(bits, 28, 20, 4, 24, False)
    rect(bits, 18, 30, 26, 4, False)
    root = Path(__file__).resolve().parents[1]
    out = root / "icon_home.bmp"
    write_bmp(out, bits)
    data = out.read_bytes()
    assert data[:2] == b"BM"
    assert struct.unpack_from("<i", data, 18)[0] == 62
    assert struct.unpack_from("<i", data, 22)[0] == 64
    assert struct.unpack_from("<H", data, 28)[0] == 1
    print(out, len(data))


if __name__ == "__main__":
    main()
