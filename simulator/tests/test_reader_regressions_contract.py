#!/usr/bin/env python3
"""Focused source contracts for the integrated real-device reader regressions."""
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


class ReaderRegressionContracts(unittest.TestCase):
    def test_reader_animation_keeps_direction_and_custom_lut_to_driver(self) -> None:
        hal = text("firmware/lib/hal/HalDisplay.cpp")
        lab = text("firmware/src/debug/M4WaveformLab.cpp")
        reader = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        animation = function_body(lab, "uint32_t runAnimateMemWindow(")
        finish = function_body(reader, "void TxtReaderActivity::finishPhysicalDisplay(")

        for call in (
            "einkDisplay.waveformLabRefresh(prev, next, lut, turnOff)",
            "einkDisplay.waveformLabRefreshWindow(prev, next, lut",
            "einkDisplay.waveformLabRefreshWindowBufs(redWin, bwWin, lut",
            "einkDisplay.waveformLabActivate(lut)",
            "einkDisplay.waveformLabActivateWindow(x, y, w, h, lut)",
            "einkDisplay.setCustomLUT(enabled, lutData)",
        ):
            self.assertIn(call, hal)
        self.assertIn("setCustomLUT(true, gLut)", animation)
        self.assertIn("stepCell(i, steps, dir, cell)", animation)
        self.assertIn("setCustomLUT(false, nullptr)", animation)
        self.assertIn("waveformLabRefresh(newFrame, newFrame, /*lut=*/nullptr", animation)
        self.assertNotRegex(animation, r"displayBuffer\s*\([^;]*(?:FULL_REFRESH|HALF_REFRESH)")
        self.assertIn("logicalToPhysicalAnimationDirection", finish)
        self.assertRegex(finish, r"runAnimateMemWindow\(oldCopy, newFrame, steps, mult, dir\)")

    def test_reader_child_handoff_drops_queued_touch_turn_without_breaking_buttons(self) -> None:
        txt = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        settings = text("firmware/src/activities/reader/EpubReaderSettingsActivity.cpp")
        settings_header = text("firmware/src/activities/reader/EpubReaderSettingsActivity.h")
        touch_policy = text("firmware/src/util/M4ListTouchPolicy.h")
        handoff = function_body(txt, "void TxtReaderActivity::cancelPendingPageTurnForChild(")
        settings_loop = function_body(settings, "void EpubReaderSettingsActivity::loop(")

        self.assertIn("pendingTurnDelta_.exchange(0", handoff)
        self.assertIn("quickMode_ = false", handoff)
        self.assertIn("cancelPendingPageTurnForChild();", txt[txt.index("void TxtReaderActivity::openMenu(") :])
        self.assertIn("touchHandoffFrames_ = 2", settings)
        self.assertIn("touchHandoffFrames_ > 0", settings_loop)
        self.assertIn("else if (mappedInput.hasTouch()", settings_loop)
        # The touch guard is deliberately narrower than the button path.
        self.assertIn("wasReleased(MappedInputManager::Button::Confirm)", settings_loop)
        self.assertIn("tap(activate) → touchDown(select)", touch_policy)
        self.assertLess(settings_loop.index("Action::Activate"),
                        settings_loop.index("Button::Confirm"))
        self.assertIn("return Action::Activate", touch_policy)

    def test_reader_size_is_canonical_and_system_face_is_runtime_scaled(self) -> None:
        settings = text("firmware/src/CrossPointSettings.h")
        persistence = text("firmware/src/CrossPointSettings.cpp")
        lists = text("firmware/src/SettingsLists.h")
        menu = text("firmware/src/activities/reader/EpubReaderMenuActivity.cpp")
        loader = text("firmware/lib/EpdFontLoader/EpdFontLoader.cpp")
        scaled = text("firmware/lib/EpdFont/ScaledEpdFont.h")

        self.assertIn("readerPixelSize = 26", settings)
        self.assertNotIn("readerPixelSize = 18", settings)
        self.assertIn('doc["readerPixelSize"]', persistence)
        self.assertIn('doc["readerPixelSize"].isNull()', persistence)
        self.assertIn("legacyReaderPixelSize", persistence)
        self.assertIn("getReaderPixelSize()", persistence)
        self.assertIn('"readerPixelSize"', lists)
        self.assertIn("SETTINGS.getReaderPixelSize()", menu)
        self.assertNotIn("customFontSize", menu)
        # Approved reader body sizes: exactly 16,24,26,36,38,40,48 (default 26)
        # 32 and 45 are excluded (74/75 kernel split)
        self.assertIn("kReaderBodyPixelSizes", settings)
        self.assertIn("kReaderBodyPixelSizes", lists)
        self.assertIn("isAllowedReaderPixelSize", settings)
        self.assertIn("snapReaderPixelSize", settings)
        self.assertIn("nextReaderPixelSize", settings)
        self.assertIn("prevReaderPixelSize", settings)
        self.assertIn("snapReaderPixelSize", persistence)
        self.assertIn("nextReaderPixelSize", menu)
        self.assertIn("prevReaderPixelSize", menu)
        # Menu must use next/prev helpers, not +-1 clamp through 32/45
        self.assertNotIn("clampReaderPixelSize", menu)
        # SettingsLists must use DynamicEnum, not continuous Value 12-48 step1
        self.assertRegex(lists, r"DynamicEnum[\s\S]{0,2000}readerPixelSize")
        self.assertNotRegex(lists, r"readerPixelSize[\s\S]{0,200}READER_PIXEL_SIZE_MIN[\s\S]{0,80}1,\s*\"readerPixelSize\"")
        # Exact allowed sizes must appear, 32 and 45 must not
        for v in ("16", "24", "26", "36", "38", "40", "48"):
            self.assertIn(v, lists)
        self.assertNotIn("\"32\"", lists)
        self.assertNotIn("\"45\"", lists)
        # 32/45 must not be in the allowed array header
        self.assertNotIn("32", settings.split("kReaderBodyPixelSizes")[1].split("}")[0] if "kReaderBodyPixelSizes" in settings else "32")
        # But explicit forbidden check via helper: ensure 32 absent from array literal
        self.assertNotIn(", 32,", settings)
        self.assertNotIn(", 45,", settings)

        # 14/15/17/20 are legacy unsupported sizes and must not be handled as enum
        for px in (14, 15, 17, 20):
            self.assertNotIn(f"== {px} ?", loader)
        self.assertIn("runtimeReaderSize = SETTINGS.getReaderPixelSize()", loader)
        self.assertIn("bindSystemReader(renderer, SETTINGS.getReaderPixelSize())", loader)
        self.assertIn("ScaledEpdFont scaledSystemReader", loader)
        self.assertNotIn("customFontSize", loader)
        self.assertNotIn("if (scale > 1.0f)", scaled)
        # Upscale must not early-return on scale >= 0.999; only exact unity
        # may reuse the source glyph. Otherwise 17/20/22 snap to the 16px face.
        self.assertIn("isUnityScale()", scaled)
        self.assertIn("scale_ >= 0.999f && scale_ <= 1.001f", scaled)
        self.assertNotRegex(scaled, r"if \(scale_ >= 0\.999f\) return")
        self.assertIn("scaledCodepoint_", scaled)
        self.assertIn("source_->getGlyph(scaledCodepoint_, style)", scaled)

    def test_settings_handoff_drains_opener_touch_edges(self) -> None:
        settings = text("firmware/src/activities/reader/EpubReaderSettingsActivity.cpp")
        settings_loop = function_body(settings, "void EpubReaderSettingsActivity::loop(")
        self.assertIn("touchHandoffFrames_ = 2", settings)
        self.assertIn("wasScreenTouchDown(dx, dy)", settings_loop)
        self.assertIn("wasScreenTapped(tx, ty)", settings_loop)
        # The drain path must run while handoff frames remain, not only after.
        drain = settings_loop[: settings_loop.index("else if (mappedInput.hasTouch()")]
        self.assertIn("touchHandoffFrames_ > 0", drain)
        self.assertIn("wasScreenTapped(tx, ty)", drain)
        self.assertIn("wasScreenTouchDown(dx, dy)", drain)

    def test_reader_settings_geometry_independent_of_custom_font(self) -> None:
        settings = text("firmware/src/activities/reader/EpubReaderSettingsActivity.cpp")
        fixed = text("firmware/src/util/M4FixedRuntimeUiFonts.h")
        # List row step comes from theme chrome metrics, not reader face metrics.
        self.assertIn("layout.rowStep = metrics.listRowHeight", settings)
        self.assertNotIn("listLineHeight", settings)
        self.assertNotIn("getReaderFontId()", settings)
        self.assertNotIn("getReaderPixelSize()", settings)
        self.assertIn("kAllowCustomChromePromotion = false", fixed)

    def test_reader_empty_first_frame_retry_is_bounded(self) -> None:
        # Empty-first-frame retries must go through the bounded policy: the old
        # branch re-armed cachedPage=-1 + updateRequired every ~10ms forever
        # (device freeze entering a 正文 whose cache has no renderable lines).
        reader = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        loop = function_body(reader, "void TxtReaderActivity::displayTaskLoop(")
        policy = text("firmware/src/util/M4TxtIndexPolicy.h")
        header = text("firmware/src/activities/reader/TxtReaderActivity.h")
        self.assertIn("emptyFirstFrameShouldRetry(emptyFirstFrameRetries_)", loop)
        self.assertIn("empty_frame_terminal", loop)
        self.assertIn("kEmptyFirstFrameMaxRetries", policy)
        self.assertIn("emptyFirstFrameRetries_ = 0", reader)
        self.assertIn("int emptyFirstFrameRetries_ = 0", header)


if __name__ == "__main__":
    unittest.main()
