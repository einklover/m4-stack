#!/usr/bin/env python3
"""Generate the production uncompressed 15x16 1-bit native-grid font blob.

The source TTF is an external input, not a repository dependency. This script
is the reproducibility boundary: given the same TTF bytes it emits a
deterministic little-endian blob (ranked BMP bitset + 15x16 bitmaps + compact
outlier sidecar). It does not implement block/radical/convolution compression.

Native-grid geometry (must stay bit-identical to the audited corpus):
  15 columns x 16 rows, 60 UPM pitch, x origin from hmtx, cell extent 74/75 UPM,
  y origin -186, 75 UPM cell, sample at cell center, filled outline -> 1.
  Packed row-major top-to-bottom, 15 bits/row contiguous = 30 bytes/glyph
  (EpdFont pixelPosition = y*width+x for width=15).

Outliers (ink outside columns 0..14) are stored as 16x16 1-bit sidecars.

Expected identity of 标准像素粗.ttf:
  font SHA-256 9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574
  28,953 strict glyphs, 32 outliers, 0 unreconstructable
  supported-corpus SHA-256 f953cc612ae2fc412bda55b4aae8a105b8d6eb105ae3c06269b84c976c2cc1b3
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.bin"
DEFAULT_HEADER = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.h"
DEFAULT_MANIFEST = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.json"

GRID_WIDTH = 15
GRID_HEIGHT = 16
OUTLIER_WIDTH = 16
RAW_BYTES = GRID_WIDTH * GRID_HEIGHT // 8  # 30
OUTLIER_BYTES = OUTLIER_WIDTH * GRID_HEIGHT // 8  # 32
HEADER_BYTES = 48
PAGE_DIR_ENTRIES = 256
LEAF_BYTES = 34  # uint16 rank_base + 32-byte occupancy
MAGIC = b"M4NG"
VERSION = 1

EXPECTED_FONT_SHA256 = "9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574"
EXPECTED_GLYPHS = 28_953
EXPECTED_OUTLIERS = 32
EXPECTED_CORPUS_SHA256 = "f953cc612ae2fc412bda55b4aae8a105b8d6eb105ae3c06269b84c976c2cc1b3"


def polygon_area(points: list[tuple[int, int]]) -> float:
    return 0.5 * sum(
        x0 * y1 - x1 * y0
        for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1])
    )


def point_in_polygon(point: tuple[float, float], points: list[tuple[int, int]]) -> bool:
    x, y = point
    inside = False
    for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
        if (y0 > y) != (y1 > y):
            if x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
                inside = not inside
    return inside


def contours(font, glyph_name: str) -> list[tuple[list[tuple[int, int]], int]]:
    glyph = font["glyf"][glyph_name]
    if glyph.numberOfContours == 0:
        return []
    coordinates, end_points, flags = glyph.getCoordinates(font["glyf"])
    if any(not bool(flag) for flag in flags):
        raise RuntimeError(f"off-curve outline in {glyph_name}")
    result = []
    start = 0
    for end in end_points:
        points = [(int(x), int(y)) for x, y in coordinates[start : end + 1]]
        result.append((points, 1 if polygon_area(points) < 0 else -1))
        start = end + 1
    return result


def choose_x_width(cs: list[tuple[list[tuple[int, int]], int]], origin: int) -> int:
    values = [x for points, _ in cs for x, _ in points]
    best = None
    for width in (74, 75):
        errors = []
        for value in values:
            start = round((value - origin) / 60)
            end = round((value - origin - width) / 60)
            errors.append(
                min(
                    abs(value - (origin + start * 60)),
                    abs(value - (origin + width + end * 60)),
                )
            )
        candidate = (max(errors), sum(error * error for error in errors), width)
        if best is None or candidate < best:
            best = candidate
    assert best
    return best[2]


def native_cells(font, glyph_name: str) -> set[tuple[int, int]]:
    cs = contours(font, glyph_name)
    _advance, origin = font["hmtx"][glyph_name]
    width = choose_x_width(cs, origin) if cs else 74
    cells: set[tuple[int, int]] = set()
    for column in range(-2, 20):
        for row in range(GRID_HEIGHT):
            x = origin + column * 60 + width / 2
            y = -186 + row * 60 + 75 / 2
            if sum(direction for points, direction in cs if point_in_polygon((x, y), points)):
                cells.add((column, row))
    return cells


def pack_bits(bits: list[int]) -> bytes:
    if len(bits) % 8 != 0:
        raise ValueError(f"bit count {len(bits)} is not a multiple of 8")
    output = bytearray()
    for offset in range(0, len(bits), 8):
        value = 0
        for bit in bits[offset : offset + 8]:
            value = (value << 1) | bit
        output.append(value)
    return bytes(output)


def pack_grid(cells: set[tuple[int, int]], width: int, height: int) -> bytes:
    # Row-major, top-to-bottom. Native row 0 is the font-space bottom; packed
    # row 0 is the glyph top so EpdFont pixelPosition=y*width+x matches.
    bits = [
        int((column, height - 1 - row) in cells)
        for row in range(height)
        for column in range(width)
    ]
    return pack_bits(bits)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def build_ranked_index(codepoints: list[int]) -> tuple[bytearray, bytearray]:
    pages: dict[int, list[int]] = {}
    for cp in codepoints:
        if cp > 0xFFFF:
            raise ValueError(f"non-BMP codepoint U+{cp:04X}")
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
    if cp > 0xFFFF:
        return None
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


def collect_corpus(font_path: Path) -> tuple[dict[int, bytes], list[tuple[int, bytes]], list[dict], dict]:
    from fontTools.ttLib import TTFont

    font = TTFont(font_path, lazy=False)
    cmap: dict[int, str] = {}
    for table in font["cmap"].tables:
        cmap.update(table.cmap)

    reconstructed_by_glyph: dict[str, dict] = {}
    supported: dict[int, bytes] = {}
    outliers: list[tuple[int, bytes]] = []
    failures: list[dict] = []

    for codepoint, glyph_name in sorted(cmap.items()):
        if glyph_name not in reconstructed_by_glyph:
            try:
                cells = native_cells(font, glyph_name)
                outside = [cell for cell in cells if not (0 <= cell[0] < GRID_WIDTH and 0 <= cell[1] < GRID_HEIGHT)]
                reconstructed_by_glyph[glyph_name] = {
                    "cells": cells,
                    "outside": outside,
                    "glyph_name": glyph_name,
                }
            except Exception as error:  # complete cmap inventory
                reconstructed_by_glyph[glyph_name] = {"error": f"{type(error).__name__}: {error}"}

        result = reconstructed_by_glyph[glyph_name]
        if "error" in result:
            failures.append(
                {"codepoint": codepoint, "glyph_name": glyph_name, "reason": result["error"]}
            )
            continue
        if result["outside"]:
            outliers.append((codepoint, pack_grid(result["cells"], OUTLIER_WIDTH, GRID_HEIGHT)))
            continue
        supported[codepoint] = pack_grid(result["cells"], GRID_WIDTH, GRID_HEIGHT)

    font_bytes = font_path.read_bytes()
    corpus_hash = hashlib.sha256(
        b"".join(struct.pack(">I", cp) + supported[cp] for cp in sorted(supported))
    ).hexdigest()
    metadata = {
        "font": str(font_path),
        "font_sha256": hashlib.sha256(font_bytes).hexdigest(),
        "font_file_bytes": len(font_bytes),
        "units_per_em": int(font["head"].unitsPerEm),
        "mapped_cmap_count": len(cmap),
        "supported_count": len(supported),
        "outlier_count": len(outliers),
        "unreconstructable_count": len(failures),
        "supported_corpus_sha256": corpus_hash,
        "grid": "15 columns x 16 rows, 60 UPM pitch, 74/75 UPM cell extent",
        "source_bitmap_encoding": (
            "row-major top-to-bottom native cells, 15 bits per row packed contiguously, 30 bytes/glyph"
        ),
        "failures": failures,
        "outlier_codepoints": [cp for cp, _ in outliers],
    }
    return supported, outliers, failures, metadata


def build_blob(supported: dict[int, bytes], outliers: list[tuple[int, bytes]]) -> bytes:
    ordered = sorted(supported)
    page_dir, leaves = build_ranked_index(ordered)
    bitmaps = b"".join(supported[cp] for cp in ordered)
    if len(bitmaps) != len(ordered) * RAW_BYTES:
        raise AssertionError(len(bitmaps))

    outlier_cps = b"".join(u16(cp) for cp, _ in outliers)
    outlier_bmps = b"".join(bmp for _, bmp in outliers)
    leaf_count = len(leaves) // LEAF_BYTES

    page_dir_off = HEADER_BYTES
    leaves_off = page_dir_off + len(page_dir)
    bitmaps_off = leaves_off + len(leaves)
    outlier_cps_off = bitmaps_off + len(bitmaps)
    outlier_bmps_off = outlier_cps_off + len(outlier_cps)

    header = bytearray(HEADER_BYTES)
    header[0:4] = MAGIC
    header[4:6] = u16(VERSION)
    header[6:8] = u16(1 if outliers else 0)
    header[8:10] = u16(len(ordered))
    header[10:12] = u16(len(outliers))
    header[12:14] = u16(leaf_count)
    header[14] = GRID_WIDTH
    header[15] = GRID_HEIGHT
    header[16] = GRID_HEIGHT  # advanceY
    header[17] = GRID_HEIGHT  # ascender
    header[18] = 0  # descender
    header[19] = 0
    header[20:24] = u32(page_dir_off)
    header[24:28] = u32(leaves_off)
    header[28:32] = u32(bitmaps_off)
    header[32:36] = u32(outlier_cps_off)
    header[36:40] = u32(outlier_bmps_off)
    header[40:42] = u16(RAW_BYTES)
    header[42:44] = u16(OUTLIER_BYTES)
    header[44:48] = u32(0)

    blob = bytes(header) + bytes(page_dir) + bytes(leaves) + bitmaps + outlier_cps + outlier_bmps
    # Self-check: ranked lookup reconstructs every strict glyph index.
    for index, cp in enumerate(ordered):
        got = lookup_rank(page_dir, leaves, cp)
        if got != index:
            raise AssertionError(f"rank mismatch U+{cp:04X}: {got} != {index}")
    if lookup_rank(page_dir, leaves, 0xFFFF) is not None and 0xFFFF not in supported:
        raise AssertionError("false positive at U+FFFF")
    return blob


def write_header(path: Path, blob: bytes, metadata: dict, occupied_pages: int) -> None:
    blob_sha = hashlib.sha256(blob).hexdigest()
    text = f"""#pragma once
// Generated by firmware/scripts/generate_m4_native_grid.py — do not edit.
// Source TTF is an external input; regenerate with --font <path-to-标准像素粗.ttf>.

#include <cstddef>
#include <cstdint>

namespace M4NativeGridFont {{

constexpr const char kMagic[4] = {{'M', '4', 'N', 'G'}};
constexpr uint16_t kVersion = {VERSION};
constexpr uint16_t kGlyphCount = {metadata["supported_count"]};
constexpr uint16_t kOutlierCount = {metadata["outlier_count"]};
constexpr uint16_t kOccupiedPages = {occupied_pages};
constexpr uint8_t kGridWidth = {GRID_WIDTH};
constexpr uint8_t kGridHeight = {GRID_HEIGHT};
constexpr uint8_t kBitmapBytes = {RAW_BYTES};
constexpr uint8_t kOutlierBitmapBytes = {OUTLIER_BYTES};
constexpr uint16_t kHeaderBytes = {HEADER_BYTES};
constexpr uint16_t kLeafBytes = {LEAF_BYTES};
constexpr size_t kBlobBytes = {len(blob)};
constexpr int kSourcePx = {GRID_HEIGHT};

constexpr const char* kFontSha256 = "{metadata["font_sha256"]}";
constexpr const char* kSupportedCorpusSha256 = "{metadata["supported_corpus_sha256"]}";
constexpr const char* kBlobSha256 = "{blob_sha}";

}}  // namespace M4NativeGridFont
"""
    path.write_text(text, encoding="utf-8", newline="\n")


def verify_expected(metadata: dict, allow_mismatch: bool) -> None:
    problems = []
    if metadata["font_sha256"] != EXPECTED_FONT_SHA256:
        problems.append(f"font SHA-256 {metadata['font_sha256']} != {EXPECTED_FONT_SHA256}")
    if metadata["supported_count"] != EXPECTED_GLYPHS:
        problems.append(f"glyph count {metadata['supported_count']} != {EXPECTED_GLYPHS}")
    if metadata["outlier_count"] != EXPECTED_OUTLIERS:
        problems.append(f"outlier count {metadata['outlier_count']} != {EXPECTED_OUTLIERS}")
    if metadata["unreconstructable_count"] != 0:
        problems.append(f"unreconstructable {metadata['unreconstructable_count']}")
    if metadata["supported_corpus_sha256"] != EXPECTED_CORPUS_SHA256:
        problems.append(
            f"corpus SHA-256 {metadata['supported_corpus_sha256']} != {EXPECTED_CORPUS_SHA256}"
        )
    if problems and not allow_mismatch:
        raise SystemExit("native-grid identity mismatch:\n  " + "\n  ".join(problems))
    if problems:
        print("WARNING identity mismatch (allowed):\n  " + "\n  ".join(problems), file=sys.stderr)


def verify_tracked_assets(blob_path: Path, header_path: Path, manifest_path: Path) -> int:
    """Confirm the committed blob/header/manifest match without the source TTF."""
    if not blob_path.is_file():
        raise SystemExit(f"native-grid blob missing: {blob_path}")
    blob = blob_path.read_bytes()
    blob_sha = hashlib.sha256(blob).hexdigest()
    header = header_path.read_text(encoding="utf-8")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    problems: list[str] = []
    if blob[:4] != MAGIC:
        problems.append(f"magic {blob[:4]!r} != {MAGIC!r}")
    if len(blob) != 874726:
        problems.append(f"blob bytes {len(blob)} != 874726")
    version, flags, glyphs, outliers, leaves = struct.unpack_from("<HHHHH", blob, 4)
    if version != VERSION:
        problems.append(f"version {version} != {VERSION}")
    if glyphs != EXPECTED_GLYPHS:
        problems.append(f"glyph count {glyphs} != {EXPECTED_GLYPHS}")
    if outliers != EXPECTED_OUTLIERS:
        problems.append(f"outlier count {outliers} != {EXPECTED_OUTLIERS}")
    if blob[14] != GRID_WIDTH or blob[15] != GRID_HEIGHT:
        problems.append(f"grid {blob[14]}x{blob[15]} != {GRID_WIDTH}x{GRID_HEIGHT}")
    if f'kBlobSha256 = "{blob_sha}"' not in header:
        problems.append("header kBlobSha256 does not match blob bytes")
    if f"kGlyphCount = {EXPECTED_GLYPHS}" not in header:
        problems.append("header glyph count mismatch")
    if f"kOutlierCount = {EXPECTED_OUTLIERS}" not in header:
        problems.append("header outlier count mismatch")
    if manifest.get("blob_sha256") != blob_sha:
        problems.append("manifest blob_sha256 does not match blob bytes")
    if manifest.get("font_sha256") != EXPECTED_FONT_SHA256:
        problems.append("manifest font_sha256 is not the audited TTF identity")
    if manifest.get("supported_corpus_sha256") != EXPECTED_CORPUS_SHA256:
        problems.append("manifest supported_corpus_sha256 is not the audited corpus")
    if manifest.get("no_epd_glyph_table") is not True:
        problems.append("manifest must record no_epd_glyph_table")
    if "EpdGlyph" in str(manifest.get("lookup", "")):
        problems.append("manifest lookup must not be a per-glyph EpdGlyph table")
    if problems:
        raise SystemExit("native-grid verify failed:\n  " + "\n  ".join(problems))
    print(
        f"native-grid verify OK ({len(blob)} bytes, sha256 {blob_sha}, "
        f"glyphs={glyphs} outliers={outliers} leaves={leaves} flags={flags})",
        file=sys.stderr,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, help="标准像素粗.ttf (or equivalent) source")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--verify",
        action="store_true",
        help="verify the tracked blob/header/manifest without the source TTF",
    )
    parser.add_argument(
        "--allow-identity-mismatch",
        action="store_true",
        help="do not fail when the TTF is not the audited 标准像素粗.ttf identity",
    )
    args = parser.parse_args()
    if args.verify:
        return verify_tracked_assets(args.output, args.header, args.manifest)
    if args.font is None:
        parser.error("--font is required unless --verify")

    font = args.font.expanduser().resolve()
    if not font.is_file():
        parser.error(f"font not found: {font}")

    print(f"sampling native 15x16 grid from {font}", file=sys.stderr)
    supported, outliers, failures, metadata = collect_corpus(font)
    print(
        f"mapped={metadata['mapped_cmap_count']} supported={metadata['supported_count']} "
        f"outliers={metadata['outlier_count']} failures={metadata['unreconstructable_count']}",
        file=sys.stderr,
    )
    verify_expected(metadata, args.allow_identity_mismatch)

    blob = build_blob(supported, outliers)
    occupied_pages = int.from_bytes(blob[12:14], "little")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    write_header(args.header, blob, metadata, occupied_pages)

    blob_sha = hashlib.sha256(blob).hexdigest()
    index_bytes = PAGE_DIR_ENTRIES * 2 + occupied_pages * LEAF_BYTES
    manifest = {
        "format": "M4NG v1 ranked-bitset + 15x16 bitmaps + 16x16 outlier sidecar",
        "blob_bytes": len(blob),
        "blob_sha256": blob_sha,
        "header_bytes": HEADER_BYTES,
        "page_dir_bytes": PAGE_DIR_ENTRIES * 2,
        "leaf_bytes": occupied_pages * LEAF_BYTES,
        "index_bytes": index_bytes,
        "bitmap_bytes": metadata["supported_count"] * RAW_BYTES,
        "outlier_index_bytes": metadata["outlier_count"] * 2,
        "outlier_bitmap_bytes": metadata["outlier_count"] * OUTLIER_BYTES,
        "occupied_pages": occupied_pages,
        "lookup": "BMP page_dir[256] uint16 leaf index (0xFFFF empty); 34-byte leaf = rank_base + 32-byte occupancy; rank = base + popcount(prefix). Outliers: sorted uint16 binary search.",
        "no_epd_glyph_table": True,
        "regenerate": "python3 firmware/scripts/generate_m4_native_grid.py --font <path-to-标准像素粗.ttf>",
        **{k: v for k, v in metadata.items() if k != "failures"},
    }
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"wrote {args.output} ({len(blob)} bytes, sha256 {blob_sha}) "
        f"index={index_bytes} occupied_pages={occupied_pages}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
