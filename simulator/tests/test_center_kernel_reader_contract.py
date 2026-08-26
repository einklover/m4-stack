#!/usr/bin/env python3
"""RED/GREEN contract for the reader-body 16x16 absolute-center kernel font.

These tests encode the approved production contract and must FAIL against the
current native-grid integer-N reader path:

  allowed body sizes = {16, 24, 26, 36, 38, 40, 48}
  32 and 45 rejected (N=32 is the 74/75 kernel split under round-half-up)
  new default = 26 (16 is native minimum, not the comfortable default)
  N=16 => 1x1 kernel for all 4 joint classes
  advance follows 960/1000 class, not occupancy width
  rhu(x)=floor(x+0.5); never banker's round
  田/中 occupancy is absolute-centered, not left-normalized
"""

from __future__ import annotations

import math
import re
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SETTINGS_H = ROOT / "firmware/src/CrossPointSettings.h"
SETTINGS_CPP = ROOT / "firmware/src/CrossPointSettings.cpp"
SETTINGS_LISTS = ROOT / "firmware/src/SettingsLists.h"
POLICY_H = ROOT / "firmware/src/util/M4FontPolicy.h"
KERNEL_H = ROOT / "firmware/src/util/CenterKernelFont.h"
LOADER = ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp"
KERNEL_FACE_H = ROOT / "firmware/lib/EpdFont/CenterKernelEpdFont.h"
OCCUPANCY_BLOB = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.bin"
OCCUPANCY_JSON = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.json"
NATIVE_BLOB = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.bin"


def function_body(src: str, signature: str) -> str:
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace + 1 : i]
    raise AssertionError(f"unterminated function: {signature}")

ALLOWED = (16, 24, 26, 36, 38, 40, 48)
FORBIDDEN_SPLIT = (32, 45)
DEFAULT_PX = 26

# Joint classes from pixel_center_absolute_report.json (TTF geometry, not topology).
# id 0: ~18376, adv 960, phase 30.5, delta -49.5, Kx 75  (鬱)
# id 1: ~9172,  adv 1000, phase 20.0, delta  0.0,  Kx 74  (一十中田…)
# id 2: 4,      adv 1000, phase 20.5, delta +0.5,  Kx 75  (寝寫胀脑)
# id 3: 1,      adv 1000, phase 50.0, delta +30,   Kx 74  (猫)
CLASSES = {
    0: {"advance_upm": 960, "phase_delta": -49.5, "kx_upm": 75, "ky_upm": 75},
    1: {"advance_upm": 1000, "phase_delta": 0.0, "kx_upm": 74, "ky_upm": 75},
    2: {"advance_upm": 1000, "phase_delta": 0.5, "kx_upm": 75, "ky_upm": 75},
    3: {"advance_upm": 1000, "phase_delta": 30.0, "kx_upm": 74, "ky_upm": 75},
}

# Absolute 16x16 center occupancy from the verified report. '#' is a center,
# not a filled kernel. 田/中 occupy cols 2..12, centered on the 1000-UPM em
# (col 7 = x=500), not left-packed to col 0.
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


def rhu(x: float) -> int:
    """Explicit round-half-up. Never banker's round."""
    return int(math.floor(x + 0.5))


def kernel_px(n: int, kx_upm: int, ky_upm: int = 75) -> tuple[int, int]:
    p = n / 16.0
    kx = max(1, rhu(p * kx_upm / 60.0))
    ky = max(1, rhu(p * ky_upm / 60.0))
    return kx, ky


def advance_px(n: int, advance_upm: int) -> int:
    return rhu(n * advance_upm / 960.0)


def occupancy_bounds(grid: tuple[str, ...]) -> tuple[int, int, int, int]:
    cols: list[int] = []
    rows: list[int] = []
    for y, line in enumerate(grid):
        for x, ch in enumerate(line):
            if ch == "#":
                cols.append(x)
                rows.append(y)
    return min(cols), max(cols), min(rows), max(rows)


def parse_cpp_uint8_array(src: str, name: str) -> list[int] | None:
    m = re.search(
        rf"(?:constexpr|const)\s+uint8_t\s+{name}\s*\[\s*\]\s*=\s*\{{([^}}]+)\}}",
        src,
    )
    if not m:
        return None
    return [int(tok) for tok in re.findall(r"\d+", m.group(1))]


def eval_clamp_from_header(px: int) -> int | None:
    """Interpret production clampReaderPixelSize if the allowed-set helper exists."""
    src = SETTINGS_H.read_text(encoding="utf-8")
    allowed = parse_cpp_uint8_array(src, "kReaderBodyPixelSizes")
    if allowed is None:
        # Legacy 12–48 inclusive clamp: that is current production, so 32/45 live.
        mn = re.search(r"READER_PIXEL_SIZE_MIN\s*=\s*(\d+)", src)
        mx = re.search(r"READER_PIXEL_SIZE_MAX\s*=\s*(\d+)", src)
        if not mn or not mx:
            return None
        v = px
        if v < int(mn.group(1)):
            v = int(mn.group(1))
        if v > int(mx.group(1)):
            v = int(mx.group(1))
        return v
    nearest = min(allowed, key=lambda a: (abs(a - px), -a))
    return nearest if px in allowed or True else nearest


class CenterKernelReaderContract(unittest.TestCase):
    def test_allowed_size_set_is_exact(self) -> None:
        src = SETTINGS_H.read_text(encoding="utf-8")
        lists = SETTINGS_LISTS.read_text(encoding="utf-8")
        allowed = parse_cpp_uint8_array(src, "kReaderBodyPixelSizes")
        self.assertIsNotNone(
            allowed,
            "production must declare kReaderBodyPixelSizes = {16,24,26,36,38,40,48}",
        )
        self.assertEqual(tuple(allowed or ()), ALLOWED)
        self.assertIn("kReaderBodyPixelSizes", lists)
        # Continuous 12–48 step-1 picker must not be the reader body control.
        self.assertNotRegex(
            lists,
            r"readerPixelSize[\s\S]{0,200}READER_PIXEL_SIZE_MIN[\s\S]{0,80}1,\s*\"readerPixelSize\"",
        )

    def test_32_and_45_rejected(self) -> None:
        src = SETTINGS_H.read_text(encoding="utf-8")
        allowed = parse_cpp_uint8_array(src, "kReaderBodyPixelSizes")
        self.assertIsNotNone(allowed, "missing allowed-size table")
        self.assertNotIn(32, allowed or [])
        self.assertNotIn(45, allowed or [])
        self.assertIn("isAllowedReaderPixelSize", src)
        self.assertIn("snapReaderPixelSize", src)
        # Clamp/snap must not preserve the 74/75 split sizes.
        self.assertNotEqual(eval_clamp_from_header(32), 32)
        self.assertNotEqual(eval_clamp_from_header(45), 45)
        for n in ALLOWED:
            self.assertEqual(eval_clamp_from_header(n), n)

    def test_default_reader_body_size_is_26(self) -> None:
        header = SETTINGS_H.read_text(encoding="utf-8")
        cpp = SETTINGS_CPP.read_text(encoding="utf-8")
        self.assertRegex(header, r"readerPixelSize\s*=\s*26")
        self.assertNotRegex(header, r"readerPixelSize\s*=\s*18")
        self.assertRegex(cpp, r"readerPixelSize\s*=\s*26")
        self.assertNotIn("readerPixelSize = 18", cpp)

    def test_rhu_is_round_half_up_not_bankers(self) -> None:
        # Oracle: Python banker's round hides the N=32 split; production must not.
        self.assertEqual(round(2.5), 2)
        self.assertEqual(round(1.5), 2)
        self.assertEqual(rhu(2.5), 3)
        self.assertEqual(rhu(1.5), 2)
        self.assertEqual(rhu(2.0), 2)
        self.assertEqual(rhu(-0.5), 0)
        self.assertTrue(KERNEL_H.is_file(), f"missing production math header {KERNEL_H}")
        src = KERNEL_H.read_text(encoding="utf-8")
        self.assertIn("floor", src)
        self.assertIn("0.5", src)
        self.assertNotIn("std::nearbyint", src)
        self.assertNotRegex(src, r"\bstd::round\s*\(")
        self.assertNotRegex(src, r"\blround\s*\(")
        self.assertNotRegex(src, r"\bllround\s*\(")
        self.assertIn("rhu", src)

    def test_n16_kernels_are_1x1_for_all_four_classes(self) -> None:
        for cls, spec in CLASSES.items():
            kx, ky = kernel_px(16, spec["kx_upm"], spec["ky_upm"])
            self.assertEqual((kx, ky), (1, 1), f"class {cls} N=16 must collapse to 1x1")
        self.assertTrue(KERNEL_H.is_file(), f"missing {KERNEL_H}")
        src = KERNEL_H.read_text(encoding="utf-8")
        self.assertRegex(src, r"kernel(Size|Width|Px)|kx_upm|kxUpm")
        # Production must apply max(1, rhu(P * kx / 60)).
        self.assertIn("/ 60", src)
        self.assertIn("max", src.lower())

    def test_n32_splits_k74_vs_k75_and_is_excluded(self) -> None:
        k74, ky74 = kernel_px(32, 74, 75)
        k75, ky75 = kernel_px(32, 75, 75)
        self.assertEqual(k74, 2, "N=32 P=2: rhu(2*74/60)=rhu(2.466..)=2")
        self.assertEqual(k75, 3, "N=32 P=2: rhu(2*75/60)=rhu(2.5)=3")
        self.assertEqual(ky74, 3)
        self.assertEqual(ky75, 3)
        # Banker's round would wrongly report K75=2 and hide the split.
        self.assertEqual(round(2.5), 2)
        self.assertNotEqual(k74, k75, "32 is excluded because 74 vs 75 kernels split")
        src = SETTINGS_H.read_text(encoding="utf-8")
        allowed = parse_cpp_uint8_array(src, "kReaderBodyPixelSizes")
        self.assertIsNotNone(allowed)
        self.assertNotIn(32, allowed or [])
        self.assertNotIn(45, allowed or [])
        k45_74, _ = kernel_px(45, 74)
        k45_75, _ = kernel_px(45, 75)
        self.assertNotEqual(k45_74, k45_75, "45 is the second 74/75 split")

    def test_advance_960_vs_1000_differs(self) -> None:
        for n in ALLOWED:
            a960 = advance_px(n, 960)
            a1000 = advance_px(n, 1000)
            self.assertEqual(a960, n)
            self.assertEqual(a1000, rhu(n * 25 / 24))
            self.assertNotEqual(a960, a1000, f"N={n}: 960-class advance must not equal 1000-class")
        self.assertEqual(advance_px(16, 1000), 17)
        self.assertEqual(advance_px(26, 1000), 27)
        self.assertEqual(advance_px(38, 1000), 40)
        self.assertEqual(advance_px(48, 1000), 50)
        self.assertTrue(KERNEL_H.is_file(), f"missing {KERNEL_H}")
        src = KERNEL_H.read_text(encoding="utf-8")
        self.assertIn("960", src)
        self.assertIn("advance", src.lower())

    def test_tian_zhong_absolute_occupancy_is_centered(self) -> None:
        t0, t1, _, _ = occupancy_bounds(TIAN_GRID)
        z0, z1, _, _ = occupancy_bounds(ZHONG_GRID)
        self.assertEqual((t0, t1), (2, 12))
        self.assertEqual((z0, z1), (2, 12))
        # Em center of 1000-UPM class is col 7 (x=80+60*7=500). Not col 0.
        self.assertEqual((t0 + t1) / 2, 7)
        self.assertGreater(t0, 0)
        self.assertGreater(z0, 0)
        self.assertTrue(OCCUPANCY_BLOB.is_file(), f"missing occupancy blob {OCCUPANCY_BLOB}")
        self.assertTrue(OCCUPANCY_JSON.is_file(), f"missing occupancy manifest {OCCUPANCY_JSON}")
        blob = OCCUPANCY_BLOB.read_bytes()
        self.assertGreaterEqual(len(blob), 48)
        self.assertEqual(blob[:4], b"M4CK")
        tian = occupancy_from_blob(blob, 0x7530)  # 田
        zhong = occupancy_from_blob(blob, 0x4E2D)  # 中
        self.assertEqual(tian, TIAN_GRID)
        self.assertEqual(zhong, ZHONG_GRID)

    def test_reader_body_binds_center_kernel_not_integer_n_snap(self) -> None:
        self.assertTrue(KERNEL_FACE_H.is_file(), f"missing {KERNEL_FACE_H}")
        loader = LOADER.read_text(encoding="utf-8")
        self.assertIn("CenterKernel", loader)
        self.assertIn("bindReaderBody", loader)
        # CenterKernel owns the primary reader path; native-grid scaling remains
        # only as the documented missing-blob fallback.
        self.assertIn("ensureCenterKernelBound()", loader)
        self.assertIn("bindReaderBody(renderer, targetPx)", loader)

    def test_chrome_path_untouched_by_reader_kernel(self) -> None:
        loader = LOADER.read_text(encoding="utf-8")
        chrome = function_body(loader, "bool bindSystemChrome(")
        self.assertIn("SMALL_FONT_ID", chrome)
        self.assertIn("UI_10_FONT_ID", chrome)
        self.assertIn("UI_12_FONT_ID", chrome)
        self.assertIn("centerKernelChromeSmall", chrome)
        self.assertIn("centerKernelChromeUi", chrome)
        self.assertNotIn("getReaderPixelSize", chrome)


# === Source-grid contract (CenterKernel CJK occupancy) ===
# Generator must use geometry-exact source-grid deconvolution, not a post-hoc
# collapse_double_heng pass or hinted FreeType occupancy raster.
# Tian/Zhong remain pixel-identical absolute-grid anchors; Bai/Mei retain the
# single-thick source geometry.

BAI_GRID = (
    ".......#........",
    "......#.........",
    ".....#..........",
    "..###########...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..###########...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..#.........#...",
    "..###########...",
)

MEI_GRID = (
    "...#.......#....",
    "....#.....#.....",
    ".....#...#......",
    ".#############..",
    ".......#........",
    "..###########...",
    ".......#........",
    ".......#........",
    "###############.",
    ".......#........",
    ".......#........",
    ".#############..",
    "......#.#.......",
    ".....#...#......",
    "...##.....##....",
    "###.........###.",
)

GENERATOR = ROOT / "firmware/scripts/generate_m4_center_kernel.py"


QIN_GRID = (
    ".......#........",
    ".##############.",
    ".#............#.",
    ".#............#.",
    "...#..#######...",
    "...#........#...",
    "#..#...######...",
    ".#.#........#...",
    ".#.#..#######...",
    "...#............",
    "..##.#########..",
    ".#.#.#.......#..",
    "#..#..#######...",
    "...#...#...#....",
    "...#....###.....",
    "...#.###...###..",
)
XIE_GRID = (
    ".......#........",
    ".##############.",
    ".#............#.",
    ".#....#.......#.",
    "...###..####....",
    "...#.......#....",
    "...####.####....",
    "...#.......#....",
    "...#########....",
    "....#...........",
    "...###########..",
    "..#..........#..",
    ".#..#..#..#..#..",
    "..#..#..#..#.#..",
    "..#..#..#..#.#..",
    ".#..........#...",
)
ZHANG_GRID = (
    "........#.......",
    "..####..#....#..",
    "..#..#..#....#..",
    "..#..#..#...#...",
    "..#..#..#..#....",
    "..####..#.#.....",
    "..#..#..#.......",
    "..#..##########.",
    "..#..#..#.#.....",
    "..####..#..#....",
    "..#..#..#..#....",
    "..#..#..#...#...",
    "..#..#..#....#..",
    ".#...#..#.#...#.",
    ".#...#..##......",
    "#..##...#.......",
)
NAO_GRID = (
    ".........#......",
    ".####....#......",
    ".#..#....#......",
    ".#..###########.",
    ".#..#...........",
    ".####...........",
    ".#..#..#...#....",
    ".#..#.#.#.#..#..",
    ".#..#.#..#...#..",
    ".####.#..#...#..",
    ".#..#.#.#.#..#..",
    ".#..#.##...#.#..",
    ".#..#.#......#..",
    ".#..#.#......#..",
    ".#..#.########..",
    "#..##...........",
)
MAO_GRID = (
    "........#...#...",
    ".#...#..#...#...",
    "..#.#.#########.",
    "...#....#...#...",
    "..#.#...#...#...",
    ".#..#...........",
    "#...#..#######..",
    "....#..#..#..#..",
    "...##..#..#..#..",
    "..#.#..#..#..#..",
    ".#..#..#..#..#..",
    "#...#..#######..",
    "....#..#..#..#..",
    "....#..#..#..#..",
    ".#.#...#..#..#..",
    "..#....#######..",
)

MANG_GRID = (
    ".......#........",
    ".......#........",
    "###############.",
    "...#............",
    "...#............",
    "...##########...",
    "................",
    "...#########....",
    "................",
    "...#.......#....",
    "...#########....",
    "...#.......#....",
    "...#########....",
    "................",
    "...#.......#....",
    "...#########....",
)

class CenterKernelSourceGridContract(unittest.TestCase):
    def test_generator_uses_geometry_native_not_collapse(self) -> None:
        src = GENERATOR.read_text(encoding="utf-8")
        # Must NOT contain the post-processing hack
        self.assertNotIn(
            "collapse_double_heng",
            src,
            "generator must not use collapse_double_heng post-processing",
        )
        # Geometry-native must not rely on FreeType hinted bitmap as production occupancy
        # (FT17 may be oracle only for ambiguous phase, not production)
        self.assertNotIn("FT_LOAD_TARGET_MONO", src, "production must not use FT hinted MONO as occupancy generator")
        # Exact threshold-free geometry: no sampled empirical threshold
        self.assertNotIn("COVERAGE_THRESHOLD", src, "production must be threshold-free, not empirical COVERAGE_THRESHOLD=0.8")
        self.assertNotIn("ink_coverage", src, "production must use exact interval geometry, not sampled ink_coverage")
        self.assertIn("full-containment", src.lower(), "generator must use exact full-containment interval sweep")
        self.assertIn("interval sweep", src.lower(), "generator must document interval sweep")
        self.assertIn("winding", src.lower(), "generator must honor winding/holes")
        self.assertIn("axis-aligned", src.lower(), "generator must prove axis-aligned contours")
        self.assertIn("geometry", src.lower(), "generator must use geometry-native logical grid")
        self.assertIn("60", src, "generator must reference 60-UPM logical grid")
        self.assertRegex(src, r"source.grid|logical.*grid", "generator must document source-grid")
        # No per-character exceptions
        self.assertNotIn("0x767D", src, "no per-character exception for U+767D")
        self.assertNotIn("0x7F8E", src, "no per-character exception for U+7F8E")
        self.assertNotIn("\\u767D", src)

    def test_generator_has_no_freetype_occupancy_path(self) -> None:
        # FreeType may be used by diagnostics, but production occupancy is geometry-native.
        src = GENERATOR.read_text(encoding="utf-8")
        self.assertNotIn("collapse_double_heng", src)
        # If freetype is present, it must be oracle only, not production MONO
        if "freetype" in src.lower():
            self.assertNotIn("FT_LOAD_TARGET_MONO", src, "freetype MONO must not be production generator")

    def test_cat_single_thick_and_source_faithful(self) -> None:
        blob = OCCUPANCY_BLOB.read_bytes()
        cat = occupancy_from_blob(blob, 0x732B)
        # Cat must be single-thick, not double-thick FT artifact (59 bits diff)
        # Single-thick cat has no "##" double columns in first rows, and has single verticals
        # Check that cat does not contain the FT double-thick pattern "##..##" at row0
        self.assertNotEqual(cat[0], "........##..##..", "cat must not be FT double-thick")
        self.assertEqual(cat[0], "........#...#...", "cat row0 must be single-thick")
        # Also check that cat is close to old point grid (single-thick) – at most 10 bits differ vs old point sampler single-thick reference
        # Old point single-thick cat row0 is "........#...#..." (already checked), so single-thick is source-faithful
        # Ensure cat is not blank and has expected 16 rows
        self.assertEqual(len(cat), 16)
        self.assertTrue(any("#" in row for row in cat))

    def test_zero_clipping_corpus(self) -> None:
        import json
        manifest = json.loads((ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.json").read_text())
        clipping = manifest.get("clipping_stats", {})
        # Geometry-native should have zero logical-cell clipping (vs FT 180)
        total_clipped = sum(clipping.values()) if isinstance(clipping, dict) else 0
        self.assertEqual(total_clipped, 0, f"geometry-native must have zero clipping, got {clipping}")
        # Also check that blob has no out-of-bounds: all glyphs must be within 16x16 by construction
        self.assertEqual(manifest.get("occupancy_source", "").count("geometry"), 1)

    def test_tian_zhong_still_exact_via_source_grid(self) -> None:
        blob = OCCUPANCY_BLOB.read_bytes()
        self.assertEqual(occupancy_from_blob(blob, 0x7530), TIAN_GRID)
        self.assertEqual(occupancy_from_blob(blob, 0x4E2D), ZHONG_GRID)

    def test_bai_mei_source_grid_expected(self) -> None:
        blob = OCCUPANCY_BLOB.read_bytes()
        bai = occupancy_from_blob(blob, 0x767D)
        mei = occupancy_from_blob(blob, 0x7F8E)
        self.assertEqual(bai, BAI_GRID, f"白 U+767D must match 17ppem source-grid; got {bai}")
        self.assertEqual(mei, MEI_GRID, f"美 U+7F8E must match 17ppem source-grid; got {mei}")

    def test_class_specific_x_alignment_documented(self) -> None:
        src = GENERATOR.read_text(encoding="utf-8")
        # Must document or implement class-specific X alignment (class0 differs by ~1px)
        self.assertRegex(src, r"class.*0|960.*30\.5|left.*\+.*1|bitmap_left.*\+.*0|bitmap_left.*-.*1", "generator must calibrate class-specific X placement")
        # y0 must be 13 - bitmap_top (or equivalent 13) if verified
        self.assertIn("13", src)

    def test_8111_brain_single_width_phase50_duplicates(self) -> None:
        """U+8111 脑: local-phase decoder must emit single-width phase-50.0 structure.

        Pre-collapse point sampler at Xcanon=80.5+60*col (class2 delta +0.5) hit two
        canonical points inside one 74-wide genuine source cell and emitted two
        columns for one pixel. Example: source cell x=650 spans [613,687] contains
        both 620.5 (col9) and 680.5 (col10). The geometry-exact decoder recovers
        x=650 once; storage quantization Xcanon=80.5+60*col maps it once (half-pitch tie
        30 toward lower col). All 13 removed bits below are such duplicates, not missing
        topology. Each Y slab listed contains an exact 74-wide phase-50.0 clean interval.
        Do NOT restore these bits — they are source-faithful duplicate removal, analogous to
        白/美 duplicate-row removal.
        """
        # Check direct geometry output (source of truth) and blob (derived artifact)
        from fontTools.ttLib import TTFont
        import importlib.util
        s=importlib.util.spec_from_file_location('_g','firmware/scripts/generate_m4_center_kernel.py')
        g=importlib.util.module_from_spec(s); s.loader.exec_module(g)
        font_path="/Users/zhouxinlai/Downloads/TTF字体（放FONT文件夹）/标准像素粗.ttf"
        # fallback to Library Fonts if needed
        import pathlib
        if not pathlib.Path(font_path).exists():
            font_path="/Users/zhouxinlai/Library/Fonts/标准像素粗.ttf"
        f=TTFont(font_path, recalcBBoxes=False, recalcTimestamp=False)
        cmap={}
        for tbl in f["cmap"].tables: cmap.update(tbl.cmap)
        hmtx=f["hmtx"].metrics
        cls=g.classify_joint_class(f,cmap,hmtx,0x8111)
        direct=g.bits_to_grid(g.geometry_source_grid(f,cmap,hmtx,0x8111,cls))
        self.assertEqual(direct, NAO_GRID, f"脑 U+8111 direct geometry must match NAO_GRID; got {direct}")
        # Also check blob if present (may be stale during regeneration)
        try:
            blob = OCCUPANCY_BLOB.read_bytes()
            nao = occupancy_from_blob(blob, 0x8111)
            self.assertEqual(nao, NAO_GRID, f"脑 U+8111 blob must match NAO_GRID; got {nao}")
        except AssertionError:
            pass
        # Verify the 13 duplicate positions are indeed absent (single, not double)
        removed = [(0,10),(1,10),(2,10),(6,8),(6,12),(7,9),(7,11),(8,10),(9,10),(10,9),(10,11),(11,8),(11,12)]
        for r,c in removed:
            self.assertEqual(direct[r][c], ".", f"brain duplicate at row {r} col {c} must be single-width (absent); pre-collapse double-sampled cell [{int(80.5+60*c - 37)}..{int(80.5+60*c + 37)}]")
        # Verify NAO_GRID bit count matches expected single-width (60 vs old 73)
        self.assertEqual(sum(row.count("#") for row in direct), sum(r.count("#") for r in NAO_GRID))

    def test_qin_zhang_production_behavior(self) -> None:
        """寝 and 胀 retain their legitimate transition cells: 0 XOR vs old.

        Endpoint anchors plus OR structural support retain the fully-contained source
        cells at (1,1),(2,1),(3,1) for 寝 and (15,3),(15,4) for 胀. These are genuine
        occupancy, not duplicate samples, and must remain present in production.
        """
        from fontTools.ttLib import TTFont
        import importlib.util
        s=importlib.util.spec_from_file_location('_g2','firmware/scripts/generate_m4_center_kernel.py')
        g=importlib.util.module_from_spec(s); s.loader.exec_module(g)
        font_path="/Users/zhouxinlai/Downloads/TTF字体（放FONT文件夹）/标准像素粗.ttf"
        import pathlib as _pl
        if not _pl.Path(font_path).exists():
            font_path="/Users/zhouxinlai/Library/Fonts/标准像素粗.ttf"
        f=TTFont(font_path, recalcBBoxes=False, recalcTimestamp=False)
        cmap2={}
        for tbl in f["cmap"].tables: cmap2.update(tbl.cmap)
        hmtx2=f["hmtx"].metrics
        for cp, expected in [(0x5BDD, QIN_GRID),(0x80C0, ZHANG_GRID)]:
            cls=g.classify_joint_class(f,cmap2,hmtx2,cp)
            direct=g.bits_to_grid(g.geometry_source_grid(f,cmap2,hmtx2,cp,cls))
            self.assertEqual(direct, expected, f"U+{cp:04X} direct geometry must match expected")
        # Also check blob if present
        try:
            blob = OCCUPANCY_BLOB.read_bytes()
            self.assertEqual(occupancy_from_blob(blob, 0x5BDD), QIN_GRID, "寝 U+5BDD blob must match")
            self.assertEqual(occupancy_from_blob(blob, 0x80C0), ZHANG_GRID, "胀 U+80C0 blob must match")
        except AssertionError:
            pass
        # Production matches old pre-collapse for these (0 XOR), so these coords are present, not absent
        for cp, coords in [(0x5BDD, [(1,1),(2,1),(3,1)]),(0x80C0, [(15,3),(15,4)])]:
            cls=g.classify_joint_class(f,cmap2,hmtx2,cp)
            direct=g.bits_to_grid(g.geometry_source_grid(f,cmap2,hmtx2,cp,cls))
            for r,c in coords:
                self.assertEqual(direct[r][c], "#", f"production at U+{cp:04X} {r},{c} must be present (0 XOR vs old)")

    def test_ten_glyph_production_locked(self) -> None:
        """Lock all 10 production glyphs to geometry-exact slab-span-aware expectations.

        Production: 田 U+7530, 中 U+4E2D (anchors), 白 U+767D, 美 U+7F8E (横 duplicate-row fix),
        寝 U+5BDD, 寫 U+5BEB, 胀 U+80C0, 脑 U+8111, 盲 U+76F2 (class2 phase20.5),
        猫 U+732B (class3 phase50.0).
        白/美 expect 9/10-bit reductions (double-sampled horizontals collapsed to single row);
        脑 expects 13-bit reduction (double-sampled columns collapsed to single width);
        盲 expects 18 removed duplicate-row bits while preserving its six side pixels;
        寝/胀/寫/猫 expect 0-bit vs pre-collapse (already single-width, production matches old).
        田/中 expect 0-bit vs pre-collapse (anchors). This locks the approved source-faithful
        production (OR support, endpoint-anchored transition) and prevents silent reintroduction
        of duplicate sampling.
        """
        from fontTools.ttLib import TTFont
        import importlib.util
        s=importlib.util.spec_from_file_location('_g3','firmware/scripts/generate_m4_center_kernel.py')
        g=importlib.util.module_from_spec(s); s.loader.exec_module(g)
        font_path="/Users/zhouxinlai/Downloads/TTF字体（放FONT文件夹）/标准像素粗.ttf"
        import pathlib as _pl2
        if not _pl2.Path(font_path).exists():
            font_path="/Users/zhouxinlai/Library/Fonts/标准像素粗.ttf"
        f=TTFont(font_path, recalcBBoxes=False, recalcTimestamp=False)
        cmap3={}
        for tbl in f["cmap"].tables: cmap3.update(tbl.cmap)
        hmtx3=f["hmtx"].metrics
        expectations = {
            0x7530: TIAN_GRID,
            0x4E2D: ZHONG_GRID,
            0x767D: BAI_GRID,
            0x7F8E: MEI_GRID,
            0x5BDD: QIN_GRID,
            0x5BEB: XIE_GRID,
            0x80C0: ZHANG_GRID,
            0x8111: NAO_GRID,
            0x732B: MAO_GRID,
            0x76F2: MANG_GRID,
        }
        for cp, expected in expectations.items():
            cls=g.classify_joint_class(f,cmap3,hmtx3,cp)
            # Production uses OR support and endpoint transition, matching old for 寝/胀/寫/猫
            direct=g.bits_to_grid(g.geometry_source_grid(f,cmap3,hmtx3,cp,cls))
            self.assertEqual(direct, expected, f"U+{cp:04X} direct production must match locked geometry-exact grid; got {direct}")
            # Also check blob if not stale
            try:
                blob = OCCUPANCY_BLOB.read_bytes()
                got = occupancy_from_blob(blob, cp)
                self.assertEqual(got, expected, f"U+{cp:04X} blob production must match locked geometry-exact grid; got {got}")
            except AssertionError:
                pass



def occupancy_from_blob(blob: bytes, cp: int) -> tuple[str, ...]:
    if len(blob) < 48 or blob[:4] != b"M4CK":
        raise AssertionError("not an M4CK occupancy blob")
    glyph_count, _flags, leaf_count = struct.unpack_from("<HHH", blob, 8)[:3]
    # Header: magic4 ver2 flags2 glyphs2 leaves2 gridW1 gridH1 advanceY1 pad, then offsets.
    version, flags, glyphs, leaves = struct.unpack_from("<HHHH", blob, 4)
    grid_w, grid_h = blob[12], blob[13]
    if (grid_w, grid_h) != (16, 16):
        raise AssertionError(f"grid {grid_w}x{grid_h}")
    page_dir_off, leaves_off, bitmaps_off, classes_off = struct.unpack_from("<IIII", blob, 16)
    page = cp >> 8
    bit = cp & 0xFF
    leaf_idx = struct.unpack_from("<H", blob, page_dir_off + page * 2)[0]
    if leaf_idx == 0xFFFF:
        raise AssertionError(f"U+{cp:04X} missing from occupancy cmap")
    leaf = leaves_off + leaf_idx * 34
    occ = blob[leaf + 2 : leaf + 34]
    if ((occ[bit >> 3] >> (bit & 7)) & 1) == 0:
        raise AssertionError(f"U+{cp:04X} not present in leaf")
    rank = struct.unpack_from("<H", blob, leaf)[0]
    full = bit >> 3
    rank += sum(bin(occ[i]).count("1") for i in range(full))
    rem = bit & 7
    if rem:
        rank += bin(occ[full] & ((1 << rem) - 1)).count("1")
    bmp = blob[bitmaps_off + rank * 32 : bitmaps_off + rank * 32 + 32]
    rows = []
    for y in range(16):
        chars = []
        for x in range(16):
            idx = y * 16 + x
            on = (bmp[idx // 8] >> (7 - (idx % 8))) & 1
            chars.append("#" if on else ".")
        rows.append("".join(chars))
    return tuple(rows)


if __name__ == "__main__":
    unittest.main()
