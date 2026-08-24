#!/usr/bin/env python3
"""Regression: selecting a custom Reader font must not alter system UI chrome.

Reproduces the real-device failure mode:
  select custom font -> open Reader Settings -> UI labels tiny / overlapped

Root cause was M4FixedRuntimeUiFonts::ensure() mapping custom TTF faces onto
SMALL/UI_10/UI_12. Chrome must stay on builtin faces; only reader/content IDs
may change.
"""

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


class UiChromeFontIsolationTests(unittest.TestCase):
    def test_custom_chrome_promotion_is_disabled(self) -> None:
        fixed = text("firmware/src/util/M4FixedRuntimeUiFonts.h")
        self.assertIn("kAllowCustomChromePromotion = false", fixed)
        ensure = function_body(fixed, "inline bool ensure(")
        self.assertIn("restore(renderer)", ensure)
        self.assertIn("return false", ensure)
        self.assertNotIn("makeFace", ensure)
        self.assertNotIn("mapFaces", ensure)
        self.assertNotIn("coversActiveUi", ensure)
        # restore may rewrite UI IDs, but only from captured builtin originals.
        restore = function_body(fixed, "inline void restore(")
        self.assertIn("o.smallRegular", restore)
        self.assertIn("o.ui10Regular", restore)
        self.assertIn("o.ui12Regular", restore)
        self.assertNotIn("TtfEpdFont", restore)

    def test_loader_restores_builtin_chrome_and_loads_custom_reader_only(self) -> None:
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        self.assertIn("M4FixedRuntimeUiFonts::restore(renderer)", loader)
        self.assertIn("UI chrome=builtin (no custom promotion)", loader)
        self.assertIn("must never replace", loader)
        # Promotion of the same custom family onto UI IDs is forbidden.
        self.assertNotRegex(
            loader,
            r"replaceFont\(\s*(UI_10_FONT_ID|UI_12_FONT_ID|SMALL_FONT_ID)\s*,\s*\*family\s*\)",
        )
        # Reader path still loads the selected custom face at readerPixelSize.
        self.assertIn("runtimeReaderSize = SETTINGS.getReaderPixelSize()", loader)
        self.assertIn("loadAndInsertCustom(renderer, d.loadCustomFamily.c_str()", loader)

    def test_m4uitext_remaps_reader_ids_only(self) -> None:
        ui = text("firmware/src/util/M4UiText.h")
        resolve = function_body(ui, "inline Face resolve(")
        self.assertIn("isReaderFontId(f.layoutFontId)", resolve)
        self.assertIn("SETTINGS.getReaderFontId()", resolve)
        # After custom selection, chrome layout IDs keep fontId == layoutFontId.
        self.assertNotIn("scaleFontToMatch", resolve)
        resolve_text = function_body(ui, "inline Face resolveForText(")
        self.assertIn("!isReaderFontId(f.layoutFontId)", resolve_text)

    def test_reader_settings_and_menu_use_chrome_ids_not_reader_face(self) -> None:
        settings = text("firmware/src/activities/reader/EpubReaderSettingsActivity.cpp")
        menu = text("firmware/src/activities/reader/EpubReaderMenuActivity.cpp")
        theme_lyra = text("firmware/src/components/themes/lyra/LyraTheme.cpp")
        theme_feng = text("firmware/src/components/themes/fengyan/FengyanTheme.cpp")

        # Settings list geometry is theme chrome metrics, not readerPixelSize.
        self.assertIn("metrics.listRowHeight", settings)
        self.assertNotIn("getReaderPixelSize()", settings)
        self.assertNotIn("getReaderFontId()", settings)

        # Style/settings labels draw through M4UiText + UI_10/UI_12.
        for blob in (menu, theme_lyra, theme_feng):
            self.assertIn("M4UiText::", blob)
            self.assertIn("UI_10_FONT_ID", blob)

        # Menu style sheet labels must not call getReaderFontId for chrome text.
        style_draw = menu[menu.index('M4UiText::draw(renderer, UI_10_FONT_ID, 20, panelTop + 27, "字体"') :]
        style_draw = style_draw[: style_draw.index("toolbarTop")]
        self.assertNotIn("getReaderFontId()", style_draw)
        self.assertNotIn("NOTOSANS_", style_draw)

    def test_ui_font_id_literals_remain_stable_constants(self) -> None:
        # Snapshot the authoritative chrome IDs. A custom hash collision into
        # these values would be a separate bug; the promotion path is gone.
        ids = text("firmware/src/fontIds.h")
        self.assertIn("#define UI_10_FONT_ID (-1246724383)", ids)
        self.assertIn("#define UI_12_FONT_ID (-359249323)", ids)
        self.assertIn("#define SMALL_FONT_ID (1073217904)", ids)

    def test_chrome_binds_builtin_native_grid_not_reader_scaler(self) -> None:
        main = text("firmware/src/main.cpp")
        policy = text("firmware/src/util/M4FontPolicy.h")
        self.assertNotIn("m4_compact_cjk", main)
        self.assertIn("kChromeSmallScale = 1", policy)
        self.assertIn("kChromeUi10Scale = 2", policy)
        self.assertIn("kChromeUi12Scale = 2", policy)
        self.assertNotIn("kChromeSmallPx = 18", policy)
        self.assertNotIn("kChromeUi10Px = 22", policy)
        self.assertNotIn("kChromeUi12Px = 26", policy)
        self.assertNotIn("chromeScale", policy)
        # Chrome is the builtin native-grid at fixed integer N, never reader px.
        self.assertIn("scaledChromeSmall.bindInteger(&nativeGridFont", main)
        self.assertIn("scaledChromeUi10.bindInteger(&nativeGridFont", main)
        self.assertIn("scaledChromeUi12.bindInteger(&nativeGridFont", main)
        self.assertIn("M4FontPolicy::kChromeSmallScale", main)
        self.assertIn("M4FontPolicy::kChromeUi10Scale", main)
        self.assertIn("M4FontPolicy::kChromeUi12Scale", main)
        self.assertIn("smallFontFamily(&scaledChromeSmall)", main)
        self.assertIn("ui10FontFamily(&scaledChromeUi10", main)
        self.assertIn("ui12FontFamily(&scaledChromeUi12", main)
        self.assertIn("M4FontPolicy::kChromeSmallPx", main)
        self.assertIn("M4FontPolicy::kChromeUi10Px", main)
        self.assertIn("M4FontPolicy::kChromeUi12Px", main)
        self.assertNotIn("getReaderPixelSize()", main)
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        bind = function_body(loader, "void bindSystemReader(")
        self.assertIn("replaceFont(NOTOSANS_16_FONT_ID", bind)
        self.assertNotIn("SMALL_FONT_ID", bind)
        self.assertNotIn("UI_10_FONT_ID", bind)
        self.assertNotIn("UI_12_FONT_ID", bind)
        self.assertNotIn("getReaderPixelSize", bind)
        # Reader builtin snaps to integer N; chrome IDs must not consume targetPx.
        self.assertIn("nativeGridIntegerScale(targetPx)", bind)
        self.assertIn("bindInteger(source, n)", bind)
        self.assertIn("source == builtinSystemReader", bind)

    def test_missing_custom_glyph_keeps_builtin_chrome(self) -> None:
        ui_text = text("firmware/src/util/M4UiText.h")
        self.assertIn("!isReaderFontId(f.layoutFontId)", ui_text)
        self.assertIn("SETTINGS.getReaderFontId()", ui_text)
        self.assertIn("renderer.hasTextGlyphs(readerFont, safeText, style)", ui_text)
        self.assertIn("f.fontId = readerFont", ui_text)
        resolve = function_body(ui_text, "inline Face resolve(")
        self.assertIn("isReaderFontId(f.layoutFontId)", resolve)
        self.assertNotIn("SMALL_FONT_ID", resolve)
        self.assertNotIn("UI_10_FONT_ID", resolve)


if __name__ == "__main__":
    unittest.main()
