#!/usr/bin/env python3
"""Contracts for runtime-reader font isolation from tiered system chrome."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


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


class RuntimeTtfUiChromeGuardTests(unittest.TestCase):
    def test_guard_reapplies_current_tier_before_boot_fallback(self) -> None:
        fixed = text("firmware/src/util/M4FixedRuntimeUiFonts.h")
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        ensure = function_body(fixed, "inline bool ensure(")
        apply_chrome = function_body(loader, "bool EpdFontLoader::applySystemChrome(")
        bind_chrome = function_body(loader, "bool bindSystemChrome(")

        self.assertIn("#include <EpdFontLoader.h>", fixed)
        self.assertRegex(ensure, r"if\s*\(\s*EpdFontLoader::applySystemChrome\(renderer\)\s*\)")
        self.assertIn("restore(renderer)", ensure)
        self.assertIn("return false", ensure)
        self.assertIn("return g_applySystemChrome ? g_applySystemChrome(renderer) : false", apply_chrome)
        self.assertIn("if (!bindCkBlob(centerKernelChromeSmall)", bind_chrome)
        self.assertIn("return false", bind_chrome)

    def test_current_tiers_are_authoritative_through_guard_path(self) -> None:
        policy = text("firmware/src/util/M4FontPolicy.h")
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        bind_chrome = function_body(loader, "bool bindSystemChrome(")

        for name, value in (
            ("kChromeUiPxSmall", 16),
            ("kChromeUiPxMedium", 24),
            ("kChromeUiPxLarge", 26),
        ):
            self.assertRegex(policy, rf"constexpr int {name} = {value};")
        self.assertIn("M4FontPolicy::chromeUiPxFromTier(SETTINGS.getUiFontSize())", bind_chrome)
        self.assertIn("bindCkFace(centerKernelChromeUi, uiPx)", bind_chrome)

    def test_contaminated_chrome_is_repaired_without_reader_promotion(self) -> None:
        fixed = text("firmware/src/util/M4FixedRuntimeUiFonts.h")
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        ensure = function_body(fixed, "inline bool ensure(")

        self.assertIn("renderer.replaceFont(SMALL_FONT_ID", fixed)
        self.assertIn("renderer.replaceFont(UI_10_FONT_ID", fixed)
        self.assertIn("renderer.replaceFont(UI_12_FONT_ID", fixed)
        self.assertNotRegex(
            loader,
            r"replaceFont\(\s*(UI_10_FONT_ID|UI_12_FONT_ID|SMALL_FONT_ID)\s*,\s*\*family\s*\)",
        )
        self.assertNotRegex(ensure, r"replaceFont\(\s*(UI_10_FONT_ID|UI_12_FONT_ID|SMALL_FONT_ID)")

    def test_legacy_epdfont_path_uses_the_same_chrome_guard(self) -> None:
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        load_fonts = function_body(loader, "bool EpdFontLoader::loadFontsFromSd(")

        self.assertIn("Preserve legacy epdfont behavior", load_fonts)
        self.assertGreaterEqual(
            load_fonts.count("M4FixedRuntimeUiFonts::ensure(renderer, d.loadCustomFamily.c_str());"),
            2,
        )
        self.assertNotRegex(
            load_fonts,
            r"replaceFont\(\s*(UI_10_FONT_ID|UI_12_FONT_ID|SMALL_FONT_ID)\s*,\s*\*family\s*\)",
        )

    def test_missing_center_kernel_keeps_boot_chrome_fallback(self) -> None:
        fixed = text("firmware/src/util/M4FixedRuntimeUiFonts.h")
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        ensure = function_body(fixed, "inline bool ensure(")
        bind_chrome = function_body(loader, "bool bindSystemChrome(")

        self.assertRegex(bind_chrome, r"if\s*\(!bindCkBlob\(centerKernelChromeSmall\).*?\)\s*\{")
        self.assertIn("return false", bind_chrome)
        self.assertLess(ensure.index("applySystemChrome"), ensure.index("restore(renderer)"))


if __name__ == "__main__":
    unittest.main()
