#!/usr/bin/env python3
"""Check the install manifest and M4-compatible tarot bitmap assets."""

import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SIZE = (112, 192)
CARD_FILES = {
    f"{index:02d}_{key}.bmp"
    for index, key in enumerate(
        (
            "fool", "magician", "priestess", "empress", "emperor", "hierophant",
            "lovers", "chariot", "strength", "hermit", "wheel", "justice",
            "hanged", "death", "temperance", "devil", "tower", "star", "moon",
            "sun", "judgement", "world",
        )
    )
}


def bmp_info(path):
    data = path.read_bytes()
    assert data[:2] == b"BM", f"{path.relative_to(ROOT)}: not a BMP"
    assert len(data) >= 62, f"{path.relative_to(ROOT)}: truncated BMP"
    file_size, pixel_offset = struct.unpack_from("<IxxxxI", data, 2)
    width, height, planes, bpp, compression, image_size = struct.unpack_from(
        "<iiHHII", data, 18
    )
    assert file_size == len(data), f"{path.relative_to(ROOT)}: file size mismatch"
    assert pixel_offset == 62, f"{path.relative_to(ROOT)}: unexpected pixel offset"
    assert planes == 1 and bpp == 1, f"{path.relative_to(ROOT)}: expected 1-bit BMP"
    assert compression == 0, f"{path.relative_to(ROOT)}: expected uncompressed BMP"
    row_size = ((width + 31) // 32) * 4
    assert image_size == row_size * abs(height), f"{path.relative_to(ROOT)}: image size mismatch"
    return width, abs(height)


def main():
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    files = manifest["files"]
    assert len(files) == len(set(files)), "manifest contains duplicate file entries"
    assert manifest["entry"] in files, "manifest entry is not packaged"
    for name in files:
        path = ROOT / name
        assert path.resolve().is_relative_to(ROOT.resolve()), f"unsafe manifest path: {name}"
        assert path.is_file(), f"manifest file missing: {name}"

    cards = {Path(name).name for name in files if name.startswith("art/") and name.endswith(".bmp")}
    assert cards == CARD_FILES | {"card_back.bmp"}, "manifest card set differs from V1 Major Arcana"
    for name in sorted(cards):
        path = ROOT / "art" / name
        size = bmp_info(path)
        assert size == RUNTIME_SIZE, f"{name}: expected {RUNTIME_SIZE}, got {size}"

    icon_size = bmp_info(ROOT / "icon_home.bmp")
    assert icon_size == (62, 64), f"icon_home.bmp: expected (62, 64), got {icon_size}"

    sheet = (ROOT / "tools" / "tarot-sprite-sheet.png").read_bytes()
    assert sheet[:8] == b"\x89PNG\r\n\x1a\n", "reference sheet is not a PNG"
    sheet_size = struct.unpack_from(">II", sheet, 16)
    assert sheet_size == (790, 1380), f"reference sheet dimensions changed: {sheet_size}"
    print("test_assets.py OK: manifest, 22 cards, card back, icon, and reference sheet")


if __name__ == "__main__":
    main()
