#!/usr/bin/env python3
"""Crop the provided pet sheet and export cache-friendly 1-bit BMP sprites."""
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "art"
SHEET = Path(__file__).with_name("pet-sprite-sheet-redrawn.png")
SIZE = 160
# (filename, left, top, right, bottom) regions include pose symbols but not neighbors.
CELLS = (
    ("egg", 10, 90, 198, 248), ("grave", 198, 90, 385, 248),
    ("baby_idle", 10, 280, 177, 472), ("baby_eat", 190, 285, 380, 472),
    ("baby_happy", 370, 270, 570, 470), ("baby_sleep", 560, 280, 750, 470),
    ("baby_sad", 744, 275, 915, 470),
    ("child_idle", 10, 500, 190, 710), ("child_eat", 185, 500, 380, 710),
    ("child_play", 372, 500, 563, 710), ("child_happy", 558, 500, 735, 710),
    ("child_sleep", 732, 520, 925, 710), ("child_sad", 938, 520, 1092, 710),
    ("child_power", 1094, 485, 1288, 710),
    ("teen_idle", 10, 720, 210, 935), ("teen_eat", 210, 735, 422, 935),
    ("teen_sleep", 432, 745, 660, 935), ("teen_sad", 665, 750, 875, 935),
    ("guard_idle", 8, 945, 220, 1170), ("guard_sleep", 225, 945, 462, 1170),
    ("guard_power", 467, 945, 705, 1172),
)


def sprite_from_region(sheet, box):
    rgba = sheet.crop(box).convert("RGBA")
    width, height = rgba.size
    alpha = rgba.getchannel("A")
    pixels = alpha.load()
    seen = set()
    keep = set()
    for y in range(height):
        for x in range(width):
            if pixels[x, y] < 24 or (x, y) in seen:
                continue
            queue = [(x, y)]
            seen.add((x, y))
            component = []
            while queue:
                px, py = queue.pop()
                component.append((px, py))
                for ny in range(max(0, py - 1), min(height, py + 2)):
                    for nx in range(max(0, px - 1), min(width, px + 2)):
                        if pixels[nx, ny] >= 24 and (nx, ny) not in seen:
                            seen.add((nx, ny))
                            queue.append((nx, ny))
            # Keep the creature and useful props (bowl, ball, grave), discard
            # tiny sparks, stray marks, and generated-sheet dust.
            if len(component) >= 300:
                keep.update(component)
    if not keep:
        raise ValueError(f"no sprite content in region {box}")
    left = min(x for x, _ in keep)
    top = min(y for _, y in keep)
    right = max(x for x, _ in keep) + 1
    bottom = max(y for _, y in keep) + 1
    mask = Image.new("L", rgba.size, 0)
    mp = mask.load()
    for x, y in keep:
        # Keep edge coverage. Making antialiased pixels fully opaque creates
        # jagged halos when the sprite is reduced to the device's 1-bit format.
        mp[x, y] = pixels[x, y]
    rgba.putalpha(mask)
    cut = rgba.crop((max(0, left - 4), max(0, top - 4),
                     min(width, right + 4), min(height, bottom + 4)))
    white = Image.new("RGBA", cut.size, (255, 255, 255, 255))
    white.alpha_composite(cut)
    return white.convert("L")


def to_1bit(gray):
    # Remove isolated marks before a single black/white threshold. A previous
    # Bayer pass made generated gray shading look like random speckle at 160px.
    gray = gray.filter(ImageFilter.MedianFilter(size=3)).filter(ImageFilter.GaussianBlur(0.45))
    pixels = gray.load()
    bits = [[0] * gray.width for _ in range(gray.height)]
    for y in range(gray.height):
        for x in range(gray.width):
            bits[y][x] = int(pixels[x, y] < 144)
    return bits


def pack_row(row):
    aligned = (len(row) + 31) // 32 * 4
    out = bytearray(aligned)
    for x, black in enumerate(row):
        if black:
            out[x >> 3] |= 0x80 >> (x & 7)
    return out


def write_bmp(path, bits):
    h, w = len(bits), len(bits[0])
    row_bytes = (w + 31) // 32 * 4
    image_size = row_bytes * h
    data_offset = 62
    header = bytearray(data_offset)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 2, data_offset + image_size)
    struct.pack_into("<I", header, 10, data_offset)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<iiHHII", header, 18, w, h, 1, 1, 0, image_size)
    struct.pack_into("<I", header, 46, 2)
    header[54:58] = bytes((255, 255, 255, 0))  # palette index 0: white
    header[58:62] = bytes((0, 0, 0, 0))        # palette index 1: black
    body = b"".join(pack_row(bits[y]) for y in range(h - 1, -1, -1))
    path.write_bytes(header + body)
    verify_bmp(path, bits)


def verify_bmp(path, expected):
    data = path.read_bytes()
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    offset = struct.unpack_from("<I", data, 10)[0]
    stride = (w + 31) // 32 * 4
    if data[:2] != b"BM" or (w, h, bpp) != (len(expected[0]), len(expected), 1):
        raise ValueError(f"invalid generated BMP header: {path}")
    if len(data) != offset + stride * h:
        raise ValueError(f"invalid generated BMP length: {path}")
    for y, row in enumerate(expected):
        encoded = data[offset + (h - 1 - y) * stride:]
        if any(bool(encoded[x >> 3] & (0x80 >> (x & 7))) != bool(black)
               for x, black in enumerate(row)):
            raise ValueError(f"generated BMP pixel mismatch: {path} at row {y}")


def make_icon(egg_bits):
    gray = Image.new("L", (SIZE, SIZE), 255)
    gray.putdata([255 if bit == 0 else 0 for row in egg_bits for bit in row])
    bbox = gray.point(lambda value: 255 if value < 248 else 0).getbbox()
    if not bbox:
        raise ValueError("egg sprite produced an empty icon")
    subject = gray.crop(bbox)
    subject.thumbnail((56, 58), Image.Resampling.LANCZOS)
    canvas = Image.new("L", (62, 64), 255)
    canvas.paste(subject, ((62 - subject.width) // 2, (64 - subject.height) // 2))
    return to_1bit(canvas)


def main():
    if not SHEET.is_file():
        raise FileNotFoundError(SHEET)
    ART.mkdir(parents=True, exist_ok=True)
    # Keep the generated sheet's transparency so the component filter can
    # distinguish the sprite from the empty background.
    sheet = Image.open(SHEET).convert("RGBA")
    if sheet.size != (1298, 1212):
        raise ValueError(f"unexpected provided sheet size: {sheet.size}")

    sprites = {}
    if sheet.size != (1298, 1212):
        raise ValueError(f"unexpected generated sheet size: {sheet.size}")
    for name, left, top, right, bottom in CELLS:
        subject = sprite_from_region(sheet, (left, top, right, bottom))
        subject.thumbnail((150, 150), Image.Resampling.LANCZOS)
        canvas = Image.new("L", (SIZE, SIZE), 255)
        canvas.paste(subject, ((SIZE - subject.width) // 2, (SIZE - subject.height) // 2))
        sprites[name] = to_1bit(canvas)
        write_bmp(ART / f"{name}.bmp", sprites[name])

    icon_bits = make_icon(sprites["egg"])
    # The drawer's 1-bit icon path renders the BMP palette with opposite
    # polarity to the plugin's gui.drawBmp path; pre-invert only this file.
    write_bmp(ROOT / "icon_home.bmp", [[1 - bit for bit in row] for row in icon_bits])
    icon_preview = Image.new("L", (62, 64), 255)
    icon_preview.putdata([255 if bit == 0 else 0 for row in icon_bits for bit in row])
    icon_preview.resize((248, 256), Image.Resampling.NEAREST).save("/tmp/pet-icon-preview.png")

    manifest = json.loads((ROOT / "manifest.json").read_text())
    missing = [f"art/{name}.bmp" for name, *_ in CELLS
               if f"art/{name}.bmp" not in manifest["files"]]
    if missing:
        raise ValueError(f"sprites missing from manifest: {missing}")

    labels = [name for name, *_ in CELLS]
    cols = 5
    cell_w, cell_h = 180, 184
    preview = Image.new("L", (cols * cell_w, ((len(labels) + cols - 1) // cols) * cell_h), 255)
    draw = ImageDraw.Draw(preview)
    font = ImageFont.load_default()
    for i, name in enumerate(labels):
        x, y = (i % cols) * cell_w + 10, (i // cols) * cell_h + 4
        im = Image.new("L", (SIZE, SIZE), 255)
        im.putdata([255 if bit == 0 else 0 for row in sprites[name] for bit in row])
        preview.paste(im, (x, y))
        draw.text((x, y + SIZE + 2), name, fill=0, font=font)
    preview.save("/tmp/pet-sprites-preview.png")
    print(f"wrote {len(sprites)} sprites; preview=/tmp/pet-sprites-preview.png")
    for name, *_ in CELLS:
        ink = sum(sum(row) for row in sprites[name])
        print(f"{name}: {(ART / (name + '.bmp')).stat().st_size} bytes, {ink} black pixels")
    print(f"icon: {(ROOT / 'icon_home.bmp').stat().st_size} bytes, 62x64")


if __name__ == "__main__":
    main()
