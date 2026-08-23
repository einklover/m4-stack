#!/usr/bin/env python3
"""Regression contracts for recovered visual-reference centering."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TTF = ROOT / "firmware/lib/EpdFont/TtfEpdFont.cpp"
CONVERTER = ROOT / "firmware/lib/EpdFont/scripts/fontconvert.py"


class VisualMetricTests(unittest.TestCase):
    def test_box_reference_is_preferred_and_advance_is_not_rewritten(self) -> None:
        runtime = TTF.read_text(encoding="utf-8")
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("{0x53E3, 0x56FD, 0x7530", runtime)
        self.assertIn("(0x53E3, 0x56FD, 0x7530", converter)
        self.assertIn("gb.xoff + visualOriginX_", runtime)
        self.assertIn("advance_x", converter)
        self.assertNotIn("gb.advance + visualOriginX_", runtime)

    def test_converter_uses_exact_pixel_mode_without_changing_legacy_default(self) -> None:
        converter = CONVERTER.read_text(encoding="utf-8")
        self.assertIn("--pixel-size", converter)
        self.assertIn("face.set_pixel_sizes(0, render_size)", converter)
        self.assertIn("face.set_char_size(size << 6, size << 6, 150, 150)", converter)


if __name__ == "__main__":
    unittest.main()
