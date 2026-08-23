#!/usr/bin/env python3
"""Regression contracts for recovered visual-reference centering."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TTF = ROOT / "firmware/lib/EpdFont/TtfEpdFont.cpp"
NORMALIZATION = ROOT / "firmware/lib/EpdFont/TtfVisualNormalization.h"
CONVERTER = ROOT / "firmware/lib/EpdFont/scripts/fontconvert.py"


class VisualMetricTests(unittest.TestCase):
    def test_box_reference_is_preferred_and_advance_is_not_rewritten(self) -> None:
        runtime = TTF.read_text(encoding="utf-8")
        normalization = NORMALIZATION.read_text(encoding="utf-8")
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("0x53E3,  // 口", normalization)
        self.assertIn("0x56FD,  // 国", normalization)
        self.assertIn("0x7530,  // 田", normalization)
        self.assertIn("scaleForReference", runtime)
        self.assertIn("renderPixelSize", runtime)
        self.assertIn("renderSizePx_", runtime)
        self.assertIn("std::sort(referenceHeights", runtime)
        self.assertNotIn("height < std::max(2, int(sizePx_) / 2)", runtime)
        self.assertIn("(0x53E3, 0x56FD, 0x7530", converter)
        self.assertIn("gb.xoff + visualOriginX_", runtime)
        self.assertIn("lookupAdvancePx(cp)", runtime)
        self.assertIn("advance_x", converter)
        self.assertNotIn("gb.advance + visualOriginX_", runtime)

    def test_reader_visual_size_does_not_rebind_reader_size_to_ui_ids(self) -> None:
        runtime = TTF.read_text(encoding="utf-8")
        loader = (ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp").read_text(encoding="utf-8")
        fixed_ui = (ROOT / "firmware/src/util/M4FixedRuntimeUiFonts.h").read_text(encoding="utf-8")
        ui_text = (ROOT / "firmware/src/util/M4UiText.h").read_text(encoding="utf-8")
        self.assertIn("runtimeReaderSize = SETTINGS.getReaderPixelSize()", loader)
        self.assertIn("bindSystemReader(renderer, SETTINGS.getReaderPixelSize())", loader)
        self.assertIn("renderer.replaceFont(NOTOSANS_16_FONT_ID", loader)
        self.assertNotIn("renderer.replaceFont(UI_10_FONT_ID, *family)", loader)
        self.assertNotIn("renderer.replaceFont(UI_12_FONT_ID, *family)", loader)
        self.assertNotIn("renderer.replaceFont(SMALL_FONT_ID, *family)", loader)
        # UI IDs, when a complete runtime family is intentionally used for
        # chrome, are still fixed native 18/22/26px faces—not the reader face
        # or readerPixelSize. The normalization applies to those faces too.
        self.assertIn("kSmallBasePx", fixed_ui)
        self.assertIn("kUi10BasePx", fixed_ui)
        self.assertIn("kUi12BasePx", fixed_ui)
        self.assertIn("renderSizePx_", runtime)
        self.assertIn("inline bool isReaderFontId", ui_text)
        self.assertIn("if (isReaderFontId(f.layoutFontId)", ui_text)
        self.assertNotIn("scaleFontToMatch(readerFontId, f.layoutFontId)", ui_text)

    def test_converter_uses_exact_pixel_mode_without_changing_legacy_default(self) -> None:
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("--pixel-size", converter)
        self.assertIn("face.set_pixel_sizes(0, render_size)", converter)
        self.assertIn("face.set_char_size(size << 6, size << 6, 150, 150)", converter)


if __name__ == "__main__":
    unittest.main()
