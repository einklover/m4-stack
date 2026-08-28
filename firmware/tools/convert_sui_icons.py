#!/usr/bin/env python3
"""Generate 1-bit SimpleUI-derived icons for the Murphy M4 firmware.

Reads tools/simpleui_icons/*.svg (ported from the KOReader simpleui.koplugin
icon set) and emits:

  * src/components/icons/simpleui_icons.h  -- C++ 32x32 1-bit bitmaps
  * tools/sui_icon_cache.py                -- Python tiles for the UI preview tools
  * build/m4ui-preview/icons/*.png         -- 32px previews + a contact sheet

Bitmap convention matches the existing icon headers (see library.h):
a byte contains 8 horizontal pixels, MSB = leftmost pixel, bit 0 = black
ink, bit 1 = transparent.
"""
from __future__ import annotations

import io
import os

from svglib.svglib import svg2rlg
from reportlab.graphics import renderPM
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "tools", "simpleui_icons")
HDR = os.path.join(REPO, "src", "components", "icons", "simpleui_icons.h")
CACHE = os.path.join(REPO, "tools", "sui_icon_cache.py")
OUT_DIR = os.path.join(REPO, "build", "m4ui-preview", "icons")

SIZE = 32
PAD = 2


def raster_gray(name: str) -> Image.Image:
    d = svg2rlg(os.path.join(SRC, name + ".svg"))
    scale = 320.0 / max(d.width, d.height)
    d.scale(scale, scale)
    d.width *= scale
    d.height *= scale
    buf = io.BytesIO()
    renderPM.drawToFile(d, buf, fmt="PNG", bg=0xFFFFFF)
    buf.seek(0)
    return Image.open(buf).convert("L")

def to_1bit_tile(img: Image.Image) -> list[list[int]]:
    """Return a SIZE x SIZE grid: 1 = ink (black), 0 = transparent."""
    # Render high-res, hard-binarize, then coverage-downsample with a plain
    # box average (no Lanczos ringing) and threshold at 50% ink coverage.
    img = img.point(lambda p: 0 if p < 128 else 255, "L")
    tiny = img.resize((SIZE, SIZE), Image.BOX).point(
        lambda p: 0 if p < 128 else 255, "L"
    )
    # 1 = ink
    return [[0 if tiny.getpixel((x, y)) < 128 else 1 for x in range(SIZE)]
            for y in range(SIZE)]

def fmt_c_header(tiles: dict[str, list[list[int]]]) -> str:
    lines = [
        "// Auto-generated -- do not edit. See tools/convert_sui_icons.py.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Ported from the KOReader simpleui.koplugin icon set (Doctor Hetfield).",
        "// 32x32 1-bit icons. Bit convention matches the existing icon headers:",
        "// byte = 8 horizontal pixels, MSB = leftmost, bit 0 = ink, bit 1 = transparent.",
        "",
    ]
    for name in sorted(tiles):
        rows = tiles[name]
        sym = name.replace("-", "_").upper() + "_ICON"
        lines.append(f"static const uint8_t {sym}[] = {{")
        lines.append(f"    // {SIZE}x{SIZE}")
        for y in range(SIZE):
            bytes_row = []
            for b in range(SIZE // 8):
                byte = 0
                for k in range(8):
                    bit = rows[y][b * 8 + k]
                    byte = (byte << 1) | bit  # tile bit already 1=transparent, 0=ink
                bytes_row.append(byte)
            lines.append("    " + ", ".join(f"0x{v:02X}" for v in bytes_row) + ",")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def fmt_py_cache(tiles: dict[str, list[list[int]]]) -> str:
    lines = [
        "# Auto-generated -- do not edit. See tools/convert_sui_icons.py.",
        '"""32x32 1-bit tiles derived from simpleui.koplugin icons.',
        "",
        "ICONS maps a snake-case name to a list of rows; each row is a list",
        "of ints where 1 = ink (black) and 0 = transparent. draw_icon() blits",
        "a tile onto a PIL grayscale canvas at any integer scale.",
        '"""',
        "",
        "ICONS = {",
    ]
    for name in sorted(tiles):
        rows = tiles[name]
        lines.append(f"    {name!r}: [")
        for row in rows:
            lines.append("        [" + ",".join(str(v) for v in row) + "],")
        lines.append("    ],")
    lines.append("}")
    return "\n".join(lines)


def main() -> None:
    names = sorted(f[:-4] for f in os.listdir(SRC) if f.endswith(".svg"))
    tiles = {name: to_1bit_tile(raster_gray(name)) for name in names}

    os.makedirs(os.path.dirname(HDR), exist_ok=True)
    os.makedirs(CACHE and os.path.dirname(CACHE), exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)

    with open(HDR, "w", encoding="utf-8") as fh:
        fh.write(fmt_c_header(tiles))
    with open(CACHE, "w", encoding="utf-8") as fh:
        fh.write(fmt_py_cache(tiles))

    sheet = Image.new("L", (len(names) * 48 + 8, 40), 255)
    for i, name in enumerate(names):
        tile = Image.new("L", (SIZE, SIZE), 255)
        for y, row in enumerate(tiles[name]):
            for x, v in enumerate(row):
                if v:
                    tile.putpixel((x, y), 0)
        tile.save(os.path.join(OUT_DIR, name + ".png"))
        sheet.paste(tile, (8 + i * 48, 4))
    sheet.save(os.path.join(OUT_DIR, "_sheet.png"))

    total = sum(len(v) * len(v[0]) for v in tiles.values())
    print(f"generated {len(names)} icons -> {HDR}, {CACHE}, {OUT_DIR}")
    print(f"  header bytes expected: {len(names)} x {SIZE}x{SIZE}/8 = {len(names) * SIZE * SIZE // 8}")
    print(f"  ink pixels total: {total}")


if __name__ == "__main__":
    main()
