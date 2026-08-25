#!/usr/bin/env python3
"""Generate the 16x16 occupancy blob (CJK center-kernel + Latin/punct).

CJK U+3400–U+9FFF: absolute-position 16x16 centers + 2-bit joint class.
All other BMP cmap glyphs: 16x16 occupancy sampled like native-grid (class 1),
so the firmware embeds one blob and does not ship m4_native_grid_15x16.bin.

Expected identity of 标准像素粗.ttf:
  font SHA-256 9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.bin"
DEFAULT_HEADER = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.h"
DEFAULT_MANIFEST = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.json"

MAGIC = b"M4CK"
VERSION = 1
HEADER_BYTES = 48
PAGE_DIR_ENTRIES = 256
LEAF_BYTES = 34
GRID = 16
OCC_BYTES = 32  # 16x16 bits
EXPECTED_FONT_SHA256 = "9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574"
EXPECTED_CJK = 27553
CJK_LO, CJK_HI = 0x3400, 0x9FFF
# Filled after --font once; --verify reads header/manifest instead of this constant.
EXPECTED_GLYPHS = None
PITCH = 60
X_BASE = 80.0
Y_WIDTH = 75
Y_ORIGIN = -186
Y_TOP = Y_ORIGIN + Y_WIDTH / 2 + (GRID - 1) * PITCH  # 751.5

# Verified joint classes (TTF geometry). Do not infer from neighbor topology.
JOINT = {
    (960, 30.5, 75): 0,
    (1000, 20.0, 74): 1,
    (1000, 20.5, 75): 2,
    (1000, 50.0, 74): 3,
}
PHASE_DELTA = {
    20.0: 0.0,
    20.5: 0.5,
    30.5: -49.5,
    50.0: 30.0,
}
X_WIDTHS = (74, 75)

TIAN = 0x7530
ZHONG = 0x4E2D
TIAN_GRID = (
    "................",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    "................",
)
ZHONG_GRID = (
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def pack_bits(bits: list[int]) -> bytes:
    if len(bits) % 8 != 0:
        raise ValueError(len(bits))
    out = bytearray()
    for offset in range(0, len(bits), 8):
        value = 0
        for bit in bits[offset : offset + 8]:
            value = (value << 1) | bit
        out.append(value)
    return bytes(out)


def contour_points(font, glyph_name: str):
    glyph = font["glyf"][glyph_name]
    coords, end_points, _ = glyph.getCoordinates(font["glyf"])
    contours = []
    start = 0
    for end in end_points:
        contours.append([(float(x), float(y)) for x, y in coords[start : end + 1]])
        start = end + 1
    return contours


def signed_area(poly):
    return sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1])) / 2.0


def point_in_polygon(x, y, poly):
    inside = False
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        if (y1 > y) != (y2 > y):
            cross = (x2 - x1) * (y - y1) / (y2 - y1) + x1
            if x < cross:
                inside = not inside
    return inside


def filled_at(contours, x, y):
    winding = 0
    for poly in contours:
        if point_in_polygon(x, y, poly):
            winding += -1 if signed_area(poly) < 0 else 1
    return winding != 0


def edge_residual(value, origin, width):
    a = abs(value - (origin + round((value - origin) / PITCH) * PITCH))
    b = abs(value - (origin + width + round((value - origin - width) / PITCH) * PITCH))
    return min(a, b)


def fit_width(xs, origin):
    scores = {}
    for width in X_WIDTHS:
        residuals = [edge_residual(v, origin, width) for v in xs]
        scores[width] = (
            max(residuals, default=0),
            sum(residuals),
            sum(r > 1e-6 for r in residuals),
        )
    return min(X_WIDTHS, key=lambda w: (*scores[w], w))


def extract_latin(font, glyph_name: str) -> bytes:
    import generate_m4_native_grid as ng

    try:
        cells = ng.native_cells(font, glyph_name)
    except Exception:
        return bytes(OCC_BYTES)
    # Pack into the CK 16x16 cell; columns outside 0..15 are clipped.
    return ng.pack_grid(cells, GRID, GRID)


def extract_glyph(font, cmap, hmtx, cp: int) -> tuple[bytes, int]:
    glyph_name = cmap[cp]
    contours = contour_points(font, glyph_name)
    xs = [x for poly in contours for x, _ in poly]
    advance, lsb = hmtx[glyph_name]
    width = fit_width(xs, lsb)
    phase = round((lsb + width / 2) % PITCH, 1)
    key = (int(advance), phase, int(width))
    if key not in JOINT:
        raise AssertionError(f"U+{cp:04X} unexpected geometry {key}")
    cls = JOINT[key]
    delta = PHASE_DELTA[phase]
    bits: list[int] = []
    for row in range(GRID):
        y = Y_TOP - row * PITCH
        for col in range(GRID):
            x = X_BASE + delta + col * PITCH
            bits.append(1 if filled_at(contours, x, y) else 0)
    return pack_bits(bits), cls


def bits_to_grid(packed: bytes) -> tuple[str, ...]:
    rows = []
    for y in range(GRID):
        chars = []
        for x in range(GRID):
            idx = y * GRID + x
            on = (packed[idx // 8] >> (7 - (idx % 8))) & 1
            chars.append("#" if on else ".")
        rows.append("".join(chars))
    return tuple(rows)


def build_ranked_index(codepoints: list[int]) -> tuple[bytearray, bytearray]:
    pages: dict[int, list[int]] = {}
    for cp in codepoints:
        pages.setdefault(cp >> 8, []).append(cp & 0xFF)
    page_dir = bytearray(b"\xff\xff" * PAGE_DIR_ENTRIES)
    leaves = bytearray()
    rank_base = 0
    leaf_index = 0
    for page in range(PAGE_DIR_ENTRIES):
        bits = pages.get(page)
        if not bits:
            continue
        occupancy = bytearray(32)
        for bit in bits:
            occupancy[bit >> 3] |= 1 << (bit & 7)
        page_dir[page * 2 : page * 2 + 2] = u16(leaf_index)
        leaves += u16(rank_base)
        leaves += occupancy
        rank_base += len(bits)
        leaf_index += 1
    if rank_base != len(codepoints):
        raise AssertionError((rank_base, len(codepoints)))
    return page_dir, leaves


def lookup_rank(page_dir: bytes, leaves: bytes, cp: int) -> int | None:
    page = cp >> 8
    bit = cp & 0xFF
    leaf_idx = page_dir[page * 2] | (page_dir[page * 2 + 1] << 8)
    if leaf_idx == 0xFFFF:
        return None
    leaf = leaves[leaf_idx * LEAF_BYTES : (leaf_idx + 1) * LEAF_BYTES]
    occupancy = leaf[2:]
    if not (occupancy[bit >> 3] >> (bit & 7)) & 1:
        return None
    rank = leaf[0] | (leaf[1] << 8)
    full_bytes = bit >> 3
    rank += sum(bin(occupancy[i]).count("1") for i in range(full_bytes))
    rem = bit & 7
    if rem:
        rank += bin(occupancy[full_bytes] & ((1 << rem) - 1)).count("1")
    return rank


def pack_classes(classes: list[int]) -> bytes:
    out = bytearray((len(classes) + 3) // 4)
    for i, cls in enumerate(classes):
        out[i >> 2] |= (cls & 3) << ((i & 3) * 2)
    return bytes(out)


def build_blob(ordered: list[int], bitmaps: list[bytes], classes: list[int]) -> bytes:
    page_dir, leaves = build_ranked_index(ordered)
    bmp = b"".join(bitmaps)
    packed_cls = pack_classes(classes)
    page_dir_off = HEADER_BYTES
    leaves_off = page_dir_off + len(page_dir)
    bitmaps_off = leaves_off + len(leaves)
    classes_off = bitmaps_off + len(bmp)
    leaf_count = len(leaves) // LEAF_BYTES
    header = bytearray(HEADER_BYTES)
    header[0:4] = MAGIC
    header[4:6] = u16(VERSION)
    header[6:8] = u16(0)
    header[8:10] = u16(len(ordered))
    header[10:12] = u16(leaf_count)
    header[12] = GRID
    header[13] = GRID
    header[14] = GRID
    header[15] = 0
    header[16:20] = u32(page_dir_off)
    header[20:24] = u32(leaves_off)
    header[24:28] = u32(bitmaps_off)
    header[28:32] = u32(classes_off)
    header[32:34] = u16(OCC_BYTES)
    header[34:36] = u16(len(packed_cls))
    blob = bytes(header) + bytes(page_dir) + bytes(leaves) + bmp + packed_cls
    for index, cp in enumerate(ordered):
        got = lookup_rank(page_dir, leaves, cp)
        if got != index:
            raise AssertionError(f"rank mismatch U+{cp:04X}: {got} != {index}")
    return blob


def collect_corpus(font_path: Path):
    from fontTools.ttLib import TTFont

    font = TTFont(str(font_path), recalcBBoxes=False, recalcTimestamp=False)
    cmap: dict[int, str] = {}
    for table in font["cmap"].tables:
        cmap.update(table.cmap)
    mapped = {cp: name for cp, name in cmap.items() if cp <= 0xFFFF}
    hmtx = font["hmtx"].metrics
    ordered = sorted(mapped)
    bitmaps: list[bytes] = []
    classes: list[int] = []
    hist: Counter[int] = Counter()
    latin_count = 0
    for i, cp in enumerate(ordered):
        if CJK_LO <= cp <= CJK_HI:
            packed, cls = extract_glyph(font, mapped, hmtx, cp)
        else:
            packed = extract_latin(font, mapped[cp])
            cls = 1
            latin_count += 1
        bitmaps.append(packed)
        classes.append(cls)
        hist[cls] += 1
        if (i + 1) % 2000 == 0:
            print(f"extracted {i + 1}/{len(ordered)}", file=sys.stderr, flush=True)
    tian = bitmaps[ordered.index(TIAN)]
    zhong = bitmaps[ordered.index(ZHONG)]
    if bits_to_grid(tian) != TIAN_GRID:
        raise AssertionError("田 occupancy mismatch vs verified absolute report")
    if bits_to_grid(zhong) != ZHONG_GRID:
        raise AssertionError("中 occupancy mismatch vs verified absolute report")
    font_bytes = font_path.read_bytes()
    corpus = hashlib.sha256(b"".join(struct.pack(">I", cp) + bitmaps[i] + bytes([classes[i]]) for i, cp in enumerate(ordered))).hexdigest()
    metadata = {
        "font": font_path.name,
        "font_sha256": hashlib.sha256(font_bytes).hexdigest(),
        "font_file_bytes": len(font_bytes),
        "format": "M4CK v1 ranked-bitset + 16x16 absolute occupancy + 2-bit joint class",
        "grid": "16x16 absolute center occupancy; not LSB/bbox left-normalized",
        "supported_count": len(ordered),
        "joint_class_histogram": {str(k): hist[k] for k in range(4)},
        "supported_corpus_sha256": corpus,
        "cjk_range": [f"U+{CJK_LO:04X}", f"U+{CJK_HI:04X}"],
        "latin_and_other_count": latin_count,
        "no_epd_glyph_table": True,
        "lookup": "BMP ranked bitset; 2-bit joint class packed by rank",
    }
    return ordered, bitmaps, classes, metadata


def write_header(path: Path, blob: bytes, metadata: dict) -> None:
    blob_sha = hashlib.sha256(blob).hexdigest()
    text = f"""#pragma once
// Generated by firmware/scripts/generate_m4_center_kernel.py — do not edit.
// Source TTF is an external input; regenerate with --font <path-to-标准像素粗.ttf>.
// Single system face: CJK center-kernel occupancy plus Latin/punct 16x16 occupancy.

#include <cstddef>
#include <cstdint>

namespace M4CenterKernelFont {{

constexpr const char kMagic[4] = {{'M', '4', 'C', 'K'}};
constexpr uint16_t kVersion = {VERSION};
constexpr uint16_t kGlyphCount = {metadata["supported_count"]};
constexpr uint8_t kGridWidth = {GRID};
constexpr uint8_t kGridHeight = {GRID};
constexpr uint8_t kBitmapBytes = {OCC_BYTES};
constexpr uint16_t kHeaderBytes = {HEADER_BYTES};
constexpr uint16_t kLeafBytes = {LEAF_BYTES};
constexpr size_t kBlobBytes = {len(blob)};
constexpr int kSourcePx = {GRID};

constexpr const char* kFontSha256 = "{metadata["font_sha256"]}";
constexpr const char* kSupportedCorpusSha256 = "{metadata["supported_corpus_sha256"]}";
constexpr const char* kBlobSha256 = "{blob_sha}";

}}  // namespace M4CenterKernelFont
"""
    path.write_text(text, encoding="utf-8", newline="\n")


def verify_tracked_assets(blob_path: Path, header_path: Path, manifest_path: Path) -> int:
    if not blob_path.is_file():
        raise SystemExit(f"center-kernel blob missing: {blob_path}")
    blob = blob_path.read_bytes()
    blob_sha = hashlib.sha256(blob).hexdigest()
    header = header_path.read_text(encoding="utf-8")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    problems: list[str] = []
    if blob[:4] != MAGIC:
        problems.append(f"magic {blob[:4]!r} != {MAGIC!r}")
    version, flags, glyphs, leaves = struct.unpack_from("<HHHH", blob, 4)
    class_bytes = struct.unpack_from("<H", blob, 34)[0]
    expected = HEADER_BYTES + PAGE_DIR_ENTRIES * 2 + leaves * LEAF_BYTES + glyphs * OCC_BYTES + class_bytes
    if len(blob) != expected:
        problems.append(f"blob bytes {len(blob)} != {expected}")
    if version != VERSION:
        problems.append(f"version {version} != {VERSION}")
    expected_glyphs = int(manifest.get("supported_count", 0))
    if expected_glyphs and glyphs != expected_glyphs:
        problems.append(f"glyph count {glyphs} != manifest {expected_glyphs}")
    if glyphs < EXPECTED_CJK:
        problems.append(f"glyph count {glyphs} < CJK floor {EXPECTED_CJK}")
    if blob[12] != GRID or blob[13] != GRID:
        problems.append(f"grid {blob[12]}x{blob[13]} != {GRID}x{GRID}")
    if f'kBlobSha256 = "{blob_sha}"' not in header:
        problems.append("header kBlobSha256 does not match blob bytes")
    if f"kGlyphCount = {glyphs}" not in header:
        problems.append("header glyph count mismatch")
    if manifest.get("blob_sha256") != blob_sha:
        problems.append("manifest blob_sha256 mismatch")
    if int(manifest.get("supported_count", 0)) != glyphs:
        problems.append("manifest glyph count mismatch")
    if int(manifest.get("latin_and_other_count", 0)) < 90:
        problems.append("latin/punct occupancy missing from blob")
    if problems:
        raise SystemExit("center-kernel identity mismatch:\n  " + "\n  ".join(problems))
    print(f"center-kernel ok glyphs={glyphs} bytes={len(blob)} sha256={blob_sha}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if args.verify:
        return verify_tracked_assets(args.output, args.header, args.manifest)
    if args.font is None:
        raise SystemExit("pass --font <path-to-标准像素粗.ttf> (or --verify)")
    ordered, bitmaps, classes, metadata = collect_corpus(args.font)
    if metadata["font_sha256"] != EXPECTED_FONT_SHA256:
        raise SystemExit(f"font SHA-256 {metadata['font_sha256']} != {EXPECTED_FONT_SHA256}")
    cjk = metadata["supported_count"] - int(metadata.get("latin_and_other_count", 0))
    if cjk != EXPECTED_CJK:
        raise SystemExit(f"CJK glyph count {cjk} != {EXPECTED_CJK}")
    hist = metadata["joint_class_histogram"]
    if hist.get("0") != 18376 or hist.get("2") != 4 or hist.get("3") != 1:
        raise SystemExit(f"CJK class hist drifted: {hist}")
    blob = build_blob(ordered, bitmaps, classes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    write_header(args.header, blob, metadata)
    occupied_pages = sum(1 for i in range(256) if blob[HEADER_BYTES + i * 2 : HEADER_BYTES + i * 2 + 2] != b"\xff\xff")
    metadata.update(
        {
            "blob_bytes": len(blob),
            "blob_sha256": hashlib.sha256(blob).hexdigest(),
            "header_bytes": HEADER_BYTES,
            "index_bytes": HEADER_BYTES + 512 + occupied_pages * LEAF_BYTES - HEADER_BYTES,
            "occupied_pages": occupied_pages,
            "bitmap_bytes": len(ordered) * OCC_BYTES,
            "class_bytes": (len(ordered) + 3) // 4,
            "regenerate": "python3 firmware/scripts/generate_m4_center_kernel.py --font <path-to-标准像素粗.ttf>",
        }
    )
    args.manifest.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.output} ({len(blob)} bytes) glyphs={len(ordered)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
