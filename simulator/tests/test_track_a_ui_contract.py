#!/usr/bin/env python3
"""Focused Track A contracts for settings defaults and system UI chrome."""
from __future__ import annotations

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


class TrackASettingsContracts(unittest.TestCase):
    def test_fresh_and_reset_defaults_are_off(self) -> None:
        header = text("firmware/src/CrossPointSettings.h")
        settings = text("firmware/src/CrossPointSettings.cpp")
        reset = function_body(settings, "void CrossPointSettings::resetToDefaults(")

        for field in ("textAntiAliasing", "systemAnimationEnabled", "pageTurnAnimationEnabled"):
            self.assertRegex(header, rf"uint8_t {field} = 0;")
            self.assertRegex(reset, rf"{field}\s*=\s*0;")

        # No settings file leaves the singleton's field initializers intact;
        # loadFromFile only returns false after JSON/binary lookup fails.
        load = function_body(settings, "bool CrossPointSettings::loadFromFile(")
        self.assertIn("return false;", load)
        self.assertNotIn("resetToDefaults();", load)

    def test_json_load_preserves_explicit_values_and_legacy_migration(self) -> None:
        settings = text("firmware/src/CrossPointSettings.cpp")
        load = function_body(settings, "bool CrossPointSettings::loadFromFile(")

        self.assertIn('pageTurnAnimationEnabled   = doc["pageTurnAnimationEnabled"]   | (uint8_t)0;', load)
        self.assertIn('textAntiAliasing         = doc["textAntiAliasing"]         | (uint8_t)0;', load)
        self.assertIn('systemAnimationEnabled = doc["systemAnimationEnabled"] | (uint8_t)0;', load)
        self.assertIn('if (doc["systemAnimationEnabled"].isNull())', load)
        self.assertIn("systemAnimationEnabled = pageTurnAnimationEnabled;", load)

        # The only fallback is the documented legacy format: old JSON used the
        # page-turn field as the global animation switch. Modern explicit keys
        # are read independently, including an explicit zero.
        modern = {"pageTurnAnimationEnabled": 1, "systemAnimationEnabled": 0, "textAntiAliasing": 1}
        self.assertEqual(modern["pageTurnAnimationEnabled"], 1)
        self.assertEqual(modern["systemAnimationEnabled"], 0)
        self.assertEqual(modern["textAntiAliasing"], 1)

    def test_binary_migration_does_not_invent_animation_values(self) -> None:
        settings = text("firmware/src/CrossPointSettings.cpp")
        binary = function_body(settings, "bool CrossPointSettings::loadFromBinaryFile(")
        self.assertNotIn("serialization::readPod(inputFile, systemAnimationEnabled)", binary)
        self.assertNotIn("serialization::readPod(inputFile, pageTurnAnimationEnabled)", binary)


class TrackAChromeContracts(unittest.TestCase):
    def test_bottom_chrome_is_plain_text_with_shared_hit_geometry(self) -> None:
        nav = text("firmware/src/util/M4TouchNavigation.cpp")
        geometry = text("firmware/src/util/TouchHitGeometry.h")
        bottom = function_body(nav, "void drawBottomBar(")

        self.assertIn('"返回"', bottom)
        self.assertIn('"主页"', bottom)
        self.assertNotIn("drawBackIcon", bottom)
        self.assertNotIn("drawHomeIcon", bottom)
        self.assertIn("makeBottomNavigationLayout", bottom)
        self.assertIn("kChapterHeaderBackWidth", geometry)
        self.assertIn("makeBottomNavigationLayout", geometry)
        self.assertIn("barHeight = 50", geometry)

    def test_chapter_selectors_share_prominent_top_back_policy(self) -> None:
        nav = text("firmware/src/util/M4TouchNavigation.cpp")
        navigation = text("firmware/src/util/M4TouchNavigation.h")
        self.assertIn('"返回"', nav)
        self.assertIn("kChapterHeaderHitWidth", navigation)
        self.assertIn("activateForChapterSelection", navigation)
        self.assertIn("chapterHeaderBackRect", nav)

        for rel in (
            "firmware/src/activities/reader/TxtReaderChapterSelectionActivity.cpp",
            "firmware/src/activities/reader/EpubReaderChapterSelectionActivity.cpp",
            "firmware/src/activities/reader/XtcReaderChapterSelectionActivity.cpp",
        ):
            source = text(rel)
            self.assertIn("activateForChapterSelection();", source)
            self.assertIn("GUI.drawHeader", source)


if __name__ == "__main__":
    unittest.main()
