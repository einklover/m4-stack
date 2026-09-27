"""Write icon_home.bmp: 62x64 1-bpp, palette 0=black 1=white, bottom-up."""

from pathlib import Path

W, H = 62, 64
STRIDE = 8  # 62 bits padded to 32-bit rows


def blank():
    return [[1 for _ in range(W)] for _ in range(H)]


def ink(px, x, y):
    if 0 <= x < W and 0 <= y < H:
        px[y][x] = 0


def rect(px, x, y, w, h, fill):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if fill or xx == x or yy == y or xx == x + w - 1 or yy == y + h - 1:
                ink(px, xx, yy)


def main():
    px = blank()
    rect(px, 2, 2, 58, 60, False)
    # 5x5 nonogram stamp with a plus, clue ticks on the top and left.
    ox, oy, cell = 18, 16, 8
    for i in range(6):
        for t in range(40):
            ink(px, ox + i * cell, oy + t)
            ink(px, ox + t, oy + i * cell)
    for r in range(5):
        for c in range(5):
            if r == 2 or c == 2:
                rect(px, ox + c * cell + 2, oy + r * cell + 2, 5, 5, True)
    for i in range(5):
        ink(px, ox + i * cell + 3, oy - 4)
        ink(px, ox + i * cell + 4, oy - 4)
        ink(px, ox - 4, oy + i * cell + 3)
        ink(px, ox - 4, oy + i * cell + 4)
    # BITMAPFILEHEADER + BITMAPINFOHEADER + 2 palette entries = 14+40+8 = 62
    pixels = bytearray()
    for y in range(H - 1, -1, -1):
        row = 0
        bit = 7
        raw = bytearray()
        for x in range(W):
            if px[y][x]:
                row |= 1 << bit
            bit -= 1
            if bit < 0:
                raw.append(row)
                row = 0
                bit = 7
        if bit != 7:
            raw.append(row)
        while len(raw) < STRIDE:
            raw.append(0)
        pixels.extend(raw)
    header = bytearray(62)
    header[0:2] = b"BM"
    size = 62 + len(pixels)
    header[2:6] = size.to_bytes(4, "little")
    header[10:14] = (62).to_bytes(4, "little")
    header[14:18] = (40).to_bytes(4, "little")
    header[18:22] = W.to_bytes(4, "little", signed=True)
    header[22:26] = H.to_bytes(4, "little", signed=True)
    header[26:28] = (1).to_bytes(2, "little")
    header[28:30] = (1).to_bytes(2, "little")
    header[34:38] = len(pixels).to_bytes(4, "little")
    # palette index 0 black, index 1 white (BGRA)
    header[54:58] = bytes((0, 0, 0, 0))
    header[58:62] = bytes((255, 255, 255, 0))
    out = Path(__file__).resolve().parents[1] / "icon_home.bmp"
    out.write_bytes(header + pixels)
    print(f"wrote {out} {out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
