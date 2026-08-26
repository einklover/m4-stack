#!/usr/bin/env python3
"""Source/asset contracts for the uncompressed 15x16 native-grid reader face."""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BLOB = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.bin"
HEADER = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.h"
MANIFEST = ROOT / "firmware/src/fontdata/m4_native_grid_15x16.json"
GENERATOR = ROOT / "firmware/scripts/generate_m4_native_grid.py"
MAIN = ROOT / "firmware/src/main.cpp"
LOADER = ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp"
POLICY = ROOT / "firmware/src/util/M4FontPolicy.h"
PLATFORMIO = ROOT / "firmware/platformio.ini"
RUNTIME = ROOT / "firmware/lib/EpdFont/NativeGridEpdFont.h"

EXPECTED_GLYPHS = 28953
EXPECTED_OUTLIERS = 32
EXPECTED_FONT_SHA256 = "9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574"
EXPECTED_CORPUS_SHA256 = "f953cc612ae2fc412bda55b4aae8a105b8d6eb105ae3c06269b84c976c2cc1b3"


class NativeGridFontTests(unittest.TestCase):
    def test_generator_does_not_contain_luna_compression(self) -> None:
        source = GENERATOR.read_text(encoding="utf-8")
        runtime = RUNTIME.read_text(encoding="utf-8")
        self.assertIn("ranked", source.lower())
        for text in (source, runtime):
            self.assertNotIn("shared-block", text)
            self.assertNotIn("shared_block", text)
            self.assertNotIn("def compress_", text)
            self.assertNotIn("blocks_from_bitmap", text)

    def test_blob_ranked_bitset_layout(self) -> None:
        blob = BLOB.read_bytes()
        self.assertGreaterEqual(len(blob), 48)
        self.assertEqual(blob[:4], b"M4NG")
        version, flags, glyphs, outliers, leaves = struct.unpack_from("<HHHHH", blob, 4)
        self.assertEqual(version, 1)
        self.assertEqual(glyphs, EXPECTED_GLYPHS)
        self.assertEqual(outliers, EXPECTED_OUTLIERS)
        grid_w, grid_h = blob[14], blob[15]
        self.assertEqual((grid_w, grid_h), (15, 16))
        page_dir_off, leaves_off, bitmaps_off, outlier_cps_off, outlier_bmps_off = struct.unpack_from(
            "<IIIII", blob, 20
        )
        bitmap_bytes, outlier_bmp_bytes = struct.unpack_from("<HH", blob, 40)
        self.assertEqual(bitmap_bytes, 30)
        self.assertEqual(outlier_bmp_bytes, 32)
        self.assertEqual(page_dir_off, 48)
        self.assertEqual(leaves_off, 48 + 512)
        self.assertEqual(bitmaps_off, leaves_off + leaves * 34)
        self.assertEqual(outlier_cps_off, bitmaps_off + glyphs * 30)
        self.assertEqual(outlier_bmps_off, outlier_cps_off + outliers * 2)
        self.assertEqual(len(blob), outlier_bmps_off + outliers * 32)

        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["supported_count"], EXPECTED_GLYPHS)
        self.assertEqual(manifest["outlier_count"], EXPECTED_OUTLIERS)
        self.assertEqual(manifest["font_sha256"], EXPECTED_FONT_SHA256)
        self.assertEqual(manifest["supported_corpus_sha256"], EXPECTED_CORPUS_SHA256)
        self.assertTrue(manifest["no_epd_glyph_table"])
        self.assertEqual(manifest["blob_sha256"], hashlib.sha256(blob).hexdigest())
        # Production index is the ranked bitset, not 16 B/glyph EpdGlyph records.
        self.assertLess(manifest["index_bytes"], 8 * 1024)
        self.assertNotIn("EpdGlyph", manifest["lookup"])

        header = HEADER.read_text(encoding="utf-8")
        self.assertIn(f"kGlyphCount = {EXPECTED_GLYPHS}", header)
        self.assertIn(manifest["blob_sha256"], header)

    def test_loader_keeps_native_grid_policy_for_reader_fallback(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        self.assertNotIn("m4_compact_cjk", main)
        self.assertNotIn("getReaderPixelSize()", main)
        loader = LOADER.read_text(encoding="utf-8")
        self.assertIn("CenterKernelEpdFont", loader)
        self.assertIn("centerKernelReader", loader)
        self.assertIn("bindReaderBody", loader)
        self.assertIn("centerKernelChromeSmall", loader)
        self.assertIn("centerKernelChromeUi", loader)
        pio = PLATFORMIO.read_text(encoding="utf-8")
        self.assertIn("src/fontdata/m4_center_kernel_16x16.bin", pio)
        self.assertIn("pre:scripts/verify_m4_center_kernel.py", pio)
        generator = GENERATOR.read_text(encoding="utf-8")
        self.assertIn("--verify", generator)
        self.assertNotIn("from fontTools.ttLib import TTFont\n", generator.split("def collect_corpus", 1)[0])

    def test_bind_scales_reader_only_from_16px_native_grid(self) -> None:
        loader = LOADER.read_text(encoding="utf-8")
        policy = POLICY.read_text(encoding="utf-8")
        self.assertIn("kNativeGridSourcePx = 16", policy)
        self.assertIn("kLogicalCellPx = 16", policy)
        self.assertIn("nativeGridIntegerScale", policy)
        self.assertIn("kChromeSmallScale = 1", policy)
        self.assertIn("kChromeUi10Scale = 1", policy)
        self.assertIn("kChromeUi12Scale = 1", policy)
        self.assertIn("kChromeUiPxMedium = 24", policy)
        self.assertNotIn("kChromeSmallPx = 18", policy)
        self.assertNotIn("kChromeUi10Px = 22", policy)
        self.assertNotIn("chromeScale", policy)
        self.assertIn("inline int systemReaderSourcePx()", policy)
        self.assertNotIn("kCompactCjkSourcePx", policy)
        bind = loader[loader.index("void bindSystemReader") : loader.index("EpdFontLoader::ensureFontsFromSd")]
        self.assertIn("nativeGridIntegerScale(targetPx)", bind)
        self.assertIn("bindInteger(source, n)", bind)
        self.assertIn("native-grid-15x16", bind)
        self.assertIn("replaceFont(NOTOSANS_16_FONT_ID", bind)
        self.assertNotIn("SMALL_FONT_ID", bind)
        self.assertNotIn("UI_10_FONT_ID", bind)
        self.assertNotIn("UI_12_FONT_ID", bind)
        self.assertNotIn("srcData->is2Bit", bind)
        self.assertNotIn("compactSource", bind)
        self.assertNotIn("kCanonicalEpdfontPixelSize", bind)
        self.assertNotIn("blitCoverage1Bit", (ROOT / "firmware/lib/EpdFont/ScaledEpdFont.h").read_text(encoding="utf-8"))

    def test_verify_mode_accepts_tracked_blob(self) -> None:
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--verify"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("native-grid verify OK", result.stderr)


if __name__ == "__main__":
    unittest.main()
