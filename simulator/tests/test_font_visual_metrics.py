#!/usr/bin/env python3
"""Regression contracts for reader metrics (not UI chrome remapping)."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TTF = ROOT / "firmware/lib/EpdFont/TtfEpdFont.cpp"
NORMALIZATION = ROOT / "firmware/lib/EpdFont/TtfVisualNormalization.h"
CONVERTER = ROOT / "firmware/lib/EpdFont/scripts/fontconvert.py"


class VisualMetricTests(unittest.TestCase):
    def test_box_reference_preserves_native_horizontal_metrics(self) -> None:
        runtime = TTF.read_text(encoding="utf-8")
        normalization = NORMALIZATION.read_text(encoding="utf-8")
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("0x53E3,  // 口", normalization)
        self.assertIn("0x56FD,  // 国", normalization)
        self.assertIn("0x7530,  // 田", normalization)
        # Production keeps nominal reader px; host helpers may still compute ratios.
        self.assertIn("Do not rewrite raster size", runtime)
        self.assertIn("renderSizePx_ = static_cast<uint16_t>(nominal)", runtime)
        self.assertIn("visualScale_ = 1.0f", runtime)
        self.assertNotIn("scaleForReference(", runtime)
        self.assertNotIn("renderPixelSize(", runtime)
        self.assertIn("scaleForReference", normalization)
        self.assertIn("(0x53E3, 0x56FD, 0x7530", converter)
        self.assertIn("glyph.left = gb.xoff;", runtime)
        self.assertNotIn("visualOriginX_", runtime)
        self.assertIn("lookupAdvancePx(cp)", runtime)
        self.assertIn("advance_x", converter)
        self.assertNotIn("gb.advance + visualOriginX_", runtime)
        # Integer-N / 16x16 cell snapping is native-grid only. TTF/OTF keeps
        # exact nominal reader pixel size and must not import that mapping.
        self.assertNotIn("nativeGridIntegerScale", runtime)
        self.assertNotIn("bindInteger", runtime)
        self.assertNotIn("kLogicalCellPx", runtime)

    def test_reader_font_never_rebinds_system_ui_ids(self) -> None:
        loader = (ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp").read_text(encoding="utf-8")
        fixed_ui = (ROOT / "firmware/src/util/M4FixedRuntimeUiFonts.h").read_text(encoding="utf-8")
        ui_text = (ROOT / "firmware/src/util/M4UiText.h").read_text(encoding="utf-8")
        self.assertIn("runtimeReaderSize = SETTINGS.getReaderPixelSize()", loader)
        self.assertIn("bindSystemReader(renderer, SETTINGS.getReaderPixelSize())", loader)
        self.assertIn("renderer.replaceFont(NOTOSANS_16_FONT_ID", loader)
        self.assertNotIn("renderer.replaceFont(UI_10_FONT_ID, *family)", loader)
        self.assertNotIn("renderer.replaceFont(UI_12_FONT_ID, *family)", loader)
        self.assertNotIn("renderer.replaceFont(SMALL_FONT_ID, *family)", loader)
        # Custom Reader fonts must never promote onto chrome IDs.
        self.assertIn("kAllowCustomChromePromotion = false", fixed_ui)
        self.assertIn("never promote a custom Reader face", fixed_ui)
        self.assertNotIn("makeFace(", fixed_ui)
        self.assertNotIn("mapFaces(", fixed_ui)
        self.assertNotIn("std::unique_ptr<TtfEpdFont>", fixed_ui)
        self.assertIn("inline bool isReaderFontId", ui_text)
        self.assertIn("if (isReaderFontId(f.layoutFontId))", ui_text)
        self.assertNotIn("scaleFontToMatch(readerFontId, f.layoutFontId)", ui_text)

    def test_converter_uses_exact_pixel_mode_without_changing_legacy_default(self) -> None:
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("--pixel-size", converter)
        self.assertIn("face.set_pixel_sizes(0, render_size)", converter)
        self.assertIn("face.set_char_size(size << 6, size << 6, 150, 150)", converter)


if __name__ == "__main__":
    unittest.main()
