#!/usr/bin/env python3
"""Source/asset contracts for the compact built-in CJK fallback."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "firmware/src/fontdata/m4_compact_cjk_16.h"
MAIN = ROOT / "firmware/src/main.cpp"
LOADER = ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp"
UI_TEXT = ROOT / "firmware/src/util/M4UiText.h"


class CompactCjkFontTests(unittest.TestCase):
    def test_asset_is_two_bit_and_in_budget(self) -> None:
        source = HEADER.read_text(encoding="utf-8")
        self.assertIn("mode: 2-bit", source)
        self.assertRegex(source, r"\n\s+true,\n};\s*$")
        bitmap = int(re.search(r"Bitmaps\[(\d+)\]", source).group(1))
        intervals = re.search(
            r"m4_compact_cjk_16Intervals\[\] = \{(.*?)\n\};", source, re.S
        ).group(1)
        rows = [
            tuple(int(value, 16) for value in row)
            for row in re.findall(r"\{ 0x([0-9A-F]+), 0x([0-9A-F]+), 0x([0-9A-F]+) \}", intervals)
        ]
        glyphs = sum(last - first + 1 for first, last, _ in rows)
        static_bytes = bitmap + glyphs * 16 + len(rows) * 12 + 32
        self.assertGreaterEqual(static_bytes, 220 * 1024)
        self.assertLessEqual(static_bytes, 270 * 1024)

    def test_loader_keeps_external_fallback_path(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        loader = LOADER.read_text(encoding="utf-8")
        self.assertIn('#include "fontdata/m4_compact_cjk_16.h"', main)
        self.assertIn("EpdFontLoader::loadFontsFromSd(renderer)", main)
        self.assertIn("promoteToReaderIds", loader)
        self.assertIn("isRuntimeTtfFamily", loader)
        self.assertIn("M4FixedRuntimeUiFonts::ensure", loader)

    def test_missing_custom_glyph_keeps_builtin_then_external_fallback(self) -> None:
        ui_text = UI_TEXT.read_text(encoding="utf-8")
        # resolveForText changes the selected face only after the candidate
        # explicitly proves coverage. A missing glyph therefore keeps the
        # compact builtin face (or the already-selected runtime reader face)
        # instead of turning into a '?' from an incomplete custom font.
        self.assertIn("EpdFontLoader::getBestFontId", ui_text)
        self.assertIn("renderer.hasTextGlyphs(uiFont, safeText, style)", ui_text)
        self.assertIn("f.fontId = uiFont", ui_text)
        self.assertIn("return f;", ui_text)


if __name__ == "__main__":
    unittest.main()
