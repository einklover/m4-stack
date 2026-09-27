#!/usr/bin/env python3
"""Crop the supplied six tarot grids into 1-bit M4 preview and detail BMPs."""

import argparse
from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "art" / "source"
PREVIEW_SIZE = (122, 192)
DETAIL_SIZE = (352, 555)
PART_SIZE = (176, 185)
TILES = {
    "tarot-gen-00-03.png": ((233, 7, 618, 615), (637, 7, 1022, 615),
                            (233, 630, 618, 1238), (637, 630, 1022, 1238)),
    "tarot-gen-04-07.png": ((232, 7, 617, 615), (637, 7, 1022, 615),
                            (232, 631, 617, 1239), (637, 631, 1022, 1239)),
    "tarot-gen-08-11.png": ((233, 7, 618, 615), (637, 7, 1022, 615),
                            (233, 631, 618, 1239), (637, 631, 1022, 1239)),
    "tarot-gen-12-15.png": ((232, 7, 617, 615), (637, 7, 1022, 615),
                            (232, 631, 617, 1239), (637, 631, 1022, 1239)),
    "tarot-gen-16-19.png": ((233, 7, 618, 615), (637, 7, 1022, 615),
                            (233, 631, 618, 1239), (637, 631, 1022, 1239)),
    "tarot-gen-20-21-back-icon.png": ((233, 7, 618, 615), (637, 7, 1022, 615),
                                      (233, 631, 618, 1239), (637, 631, 1022, 1239)),
}
CARDS = (
    "00_fool", "01_magician", "02_priestess", "03_empress", "04_emperor",
    "05_hierophant", "06_lovers", "07_chariot", "08_strength", "09_hermit",
    "10_wheel", "11_justice", "12_hanged", "13_death", "14_temperance",
    "15_devil", "16_tower", "17_star", "18_moon", "19_sun",
    "20_judgement", "21_world",
)


def card_image(source: Path, box, size):
    image = Image.open(source).convert("L").crop(box)
    image = ImageOps.fit(image, size, method=Image.Resampling.LANCZOS)
    return image.point(lambda value: 255 if value >= 160 else 0, mode="1")


def save_bmp(image, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="BMP")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT,
                        help="output root (defaults to the plugin directory)")
    parser.add_argument("--only", type=int, choices=range(22),
                        help="generate one card and its slices for the QEMU probe")
    args = parser.parse_args()
    output = args.output.resolve()
    selected = set(range(22)) if args.only is None else {args.only}

    card_index = 0
    back_crop = None
    icon_crop = None
    for filename, boxes in TILES.items():
        source = SOURCE / filename
        if not source.is_file():
            raise SystemExit(f"missing supplied source: {source}")
        grid = Image.open(source)
        if grid.size != (1254, 1254):
            raise SystemExit(f"unexpected source dimensions: {source.name} {grid.size}")
        for box in boxes:
            if card_index < 22:
                if card_index in selected:
                    stem = CARDS[card_index]
                    preview = card_image(source, box, PREVIEW_SIZE)
                    detail = card_image(source, box, DETAIL_SIZE)
                    save_bmp(preview, output / "art" / f"{stem}.bmp")
                    part_w, part_h = PART_SIZE
                    for part in range(6):
                        col = part % 2
                        row = part // 2
                        crop = detail.crop((col * part_w, row * part_h,
                                            (col + 1) * part_w, (row + 1) * part_h))
                        save_bmp(crop, output / "art" / "detail" / f"{stem}_{part}.bmp")
                card_index += 1
            elif card_index == 22:
                back_crop = (source, box)
                card_index += 1
            else:
                icon_crop = (source, box)
                card_index += 1

    if args.only is None:
        if card_index != 24 or back_crop is None or icon_crop is None:
            raise SystemExit(f"expected 22 cards + back + icon, got {card_index} tiles")
        save_bmp(card_image(*back_crop, PREVIEW_SIZE), output / "art" / "card_back.bmp")
        icon = card_image(*icon_crop, (62, 64))
        # Preserve the source icon's black and white pixels in the app icon.
        save_bmp(icon, output / "icon_home.bmp")
    print(f"generated {len(selected)} card(s); detail raster {DETAIL_SIZE[0]}x{DETAIL_SIZE[1]}")


if __name__ == "__main__":
    main()
