#!/usr/bin/env python3
"""Validate supplied tarot masters and their M4 1-bit BMP derivatives."""

import hashlib
import json
import struct
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
PREVIEW_SIZE = (122, 192)
DETAIL_PART_SIZE = (176, 185)
CARD_STEMS = (
    "00_fool", "01_magician", "02_priestess", "03_empress", "04_emperor",
    "05_hierophant", "06_lovers", "07_chariot", "08_strength", "09_hermit",
    "10_wheel", "11_justice", "12_hanged", "13_death", "14_temperance",
    "15_devil", "16_tower", "17_star", "18_moon", "19_sun",
    "20_judgement", "21_world",
)
MASTER_HASHES = {
    "tarot-gen-00-03.png": "fee40bf26ab2563002f44c368626e62e867c5fa70639354221cc4eff96de71d0",
    "tarot-gen-04-07.png": "79e1129c12be13461615528af7d1cc13e8bce7cf146d8bd81ea692e1bfd96cdc",
    "tarot-gen-08-11.png": "51f70d0ade97a85e20e63e6eb13dd8053a9d06b9c0d60ae022fd01db2f2c4237",
    "tarot-gen-12-15.png": "2b9ae68f2d8eea672994472412131a73ae1a1e3ae5300899bc0571521e7871b5",
    "tarot-gen-16-19.png": "db166dd7206f7dce4dbb60f279d887f409ac66e84fd951e2a2935a376cc20238",
    "tarot-gen-20-21-back-icon.png": "8c11fdcb996ec400b8576e96a020c68f2cc361845e217ca1f8d78ad5b94c1091",
}


def bmp_info(path):
    data = path.read_bytes()
    assert data[:2] == b"BM", f"{path.relative_to(ROOT)}: not a BMP"
    assert len(data) >= 62, f"{path.relative_to(ROOT)}: truncated BMP"
    file_size = struct.unpack_from("<I", data, 2)[0]
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
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
    assert (manifest["version"], manifest["versionCode"]) == ("0.1.3", 4)
    files = manifest["files"]
    assert len(files) == len(set(files)), "manifest contains duplicate file entries"
    assert manifest["entry"] in files, "manifest entry is not packaged"
    for name in files:
        path = ROOT / name
        assert path.resolve().is_relative_to(ROOT.resolve()), f"unsafe manifest path: {name}"
        assert path.is_file(), f"manifest file missing: {name}"

    expected_previews = {f"art/{stem}.bmp" for stem in CARD_STEMS} | {"art/card_back.bmp"}
    expected_details = {
        f"art/detail/{stem}_{part}.bmp" for stem in CARD_STEMS for part in range(6)
    }
    actual_previews = {
        name for name in files
        if name.startswith("art/") and Path(name).parent == Path("art") and name.endswith(".bmp")
    }
    actual_details = {name for name in files if name.startswith("art/detail/")}
    assert actual_previews == expected_previews, "manifest card/back set differs from 22-card V1"
    assert actual_details == expected_details, "manifest detail set differs from 22 cards x 6 slices"
    assert len(actual_previews) == 23 and len(actual_details) == 132
    for name in sorted(actual_previews):
        size = bmp_info(ROOT / name)
        assert size == PREVIEW_SIZE, f"{name}: wrong preview size"
        assert max(size) <= 192, f"{name}: exceeds gui.drawBmp edge limit"
    for name in sorted(actual_details):
        size = bmp_info(ROOT / name)
        assert size == DETAIL_PART_SIZE, f"{name}: wrong detail slice size"
        assert max(size) <= 192, f"{name}: exceeds gui.drawBmp edge limit"
    assert bmp_info(ROOT / "icon_home.bmp") == (62, 64)

    source_dir = ROOT / "art" / "source"
    assert {p.name for p in source_dir.glob("*.png")} == set(MASTER_HASHES)
    for name, expected_hash in MASTER_HASHES.items():
        data = (source_dir / name).read_bytes()
        assert hashlib.sha256(data).hexdigest() == expected_hash, f"source changed: {name}"
        with Image.open(source_dir / name) as image:
            assert image.size == (1254, 1254) and image.mode == "RGB", f"bad source image: {name}"
    assert not (source_dir / "tarot-generated-eink-22-atlas.png").exists()
    assert not (ROOT / "tools" / "make_art.py").exists(), "obsolete simple-art generator remains"
    print("test_assets.py OK: six supplied masters, 22 cards + back, 1-bit previews and 132 slices")


if __name__ == "__main__":
    main()
