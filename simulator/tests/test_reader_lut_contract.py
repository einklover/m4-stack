#!/usr/bin/env python3
"""Source contracts for the reader page-turn LUT/waveform policy.

Locks the completed-audit product rules at source level:
  - the reader page-turn animation arms the requested LUT on the driver and
    disarms it before the stock same-frame seed (no FAST fallback, no
    re-armed waveform after settle);
  - forward/backward turns share one windowed-LUT path driven by the
    configured logical direction;
  - ordinary UI surfaces never request FULL/HALF waveforms;
  - reader-body cleanup is the explicit single-inversion
    READER_CLEANUP_REFRESH + READER_BODY_CONTEXT path only.
Host/static only: no QEMU, ADB, hardware, or simulator journeys.
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
    raise AssertionError(f"unterminated function {signature}")


class PageTurnLutContracts(unittest.TestCase):
    LAB = "firmware/src/debug/M4WaveformLab.cpp"
    TXT = "firmware/src/activities/reader/TxtReaderActivity.cpp"

    def test_animation_arms_lut_before_first_activation(self) -> None:
        body = function_body(text(self.LAB), "uint32_t runAnimateMemWindow(")
        arm = body.index("gDisplay->setCustomLUT(true, gLut);")
        self.assertLess(
            arm,
            body.index("gDisplay->waveformLabActivate(gLut);"),
            "driver must hold the requested LUT before any activation",
        )

    def test_animation_disarms_lut_before_stock_seed(self) -> None:
        body = function_body(text(self.LAB), "uint32_t runAnimateMemWindow(")
        disarm = body.rindex("gDisplay->setCustomLUT(false, nullptr);")
        seed = body.index(
            "gDisplay->waveformLabRefresh(newFrame, newFrame, /*lut=*/nullptr"
        )
        self.assertLess(
            disarm,
            seed,
            "stock same-frame seed must never re-arm the experiment waveform",
        )
        self.assertNotIn("gLut", body[seed + len("gDisplay->waveformLabRefresh(newFrame") :])

    def test_animation_never_falls_back_to_full_or_half(self) -> None:
        body = function_body(text(self.LAB), "uint32_t runAnimateMemWindow(")
        executable = "\n".join(line.split("//", 1)[0] for line in body.splitlines())
        self.assertIsNone(
            re.search(r"displayBuffer\s*\([^;]*(?:FULL_REFRESH|HALF_REFRESH)", executable),
            "page-turn wipe must not end in a strong multi-flash refresh",
        )

    def test_forward_and_backward_share_one_windowed_lut_path(self) -> None:
        finish = function_body(text(self.TXT), "void TxtReaderActivity::finishPhysicalDisplay(")
        # Direction comes from the configured logical direction mapped through
        # the shared orientation transform — not from turn sign. Both prev and
        # next flow through this same call site.
        self.assertIn("SETTINGS.pageTurnAnimationDir", finish)
        self.assertIn("renderer.logicalToPhysicalAnimationDirection(logicalDir)", finish)
        self.assertRegex(finish, r"runAnimateMemWindow\(oldCopy, newFrame, steps, mult, dir\)")
        # The failure fallback is a plain fast frame, not a strong waveform.
        self.assertRegex(fallback := finish, r"\[PTA\] anim failed")
        self.assertNotIn("HalDisplay::FULL_REFRESH", fallback)

    def test_reader_cadence_uses_explicit_cleanup_mode_only(self) -> None:
        txt = text(self.TXT)
        cadence = txt.count(
            "HalDisplay::READER_CLEANUP_REFRESH, HalDisplay::READER_BODY_CONTEXT"
        )
        self.assertEqual(cadence, 2, "single-page and dual-page cleanup call sites only")
        self.assertIsNone(
            re.search(r"displayBuffer\s*\([^;]*HalDisplay::HALF_REFRESH", txt),
            "reader must not use HALF as automatic cleanup",
        )
        self.assertIsNone(
            re.search(r"displayBuffer\s*\([^;]*HalDisplay::FULL_REFRESH", txt),
            "reader must not use FULL as automatic cleanup",
        )


class UiNeverFullRefreshContracts(unittest.TestCase):
    UI_PATHS = (
        "firmware/src/main.cpp",
        "firmware/src/activities/home/HomeActivity.cpp",
        "firmware/src/activities/home/MyLibraryActivity.cpp",
        "firmware/src/activities/boot_sleep/SleepActivity.cpp",
        "firmware/src/activities/settings/SettingsActivity.cpp",
        "firmware/src/activities/reader/EpubReaderMenuActivity.cpp",
        "firmware/src/activities/reader/EpubReaderSettingsActivity.cpp",
        "firmware/src/activities/reader/BookmarkManagerActivity.cpp",
        "firmware/src/activities/reader/TxtReaderChapterSelectionActivity.cpp",
        "firmware/src/debug/M4WaveformLab.cpp",
    )

    def test_ui_surfaces_do_not_request_strong_waveforms(self) -> None:
        for rel in self.UI_PATHS:
            src = text(rel)
            self.assertIsNone(
                re.search(
                    r"(?:displayBuffer|refreshDisplay)\s*\([^;]*?(?:FULL_REFRESH|HALF_REFRESH)",
                    src,
                ),
                msg=rel,
            )

    def test_renderer_boundary_demotes_everything_but_reader_cleanup(self) -> None:
        gfx_body = function_body(
            text("firmware/lib/GfxRenderer/GfxRenderer.cpp"),
            "void GfxRenderer::displayBuffer(",
        )
        self.assertIn("READER_BODY_CONTEXT", gfx_body)
        self.assertIn("? HalDisplay::READER_CLEANUP_REFRESH", gfx_body)
        self.assertIn(": HalDisplay::FAST_REFRESH", gfx_body)
        hal_body = function_body(
            text("firmware/lib/hal/HalDisplay.cpp"),
            "HalDisplay::RefreshMode normalizeRefreshMode(",
        )
        self.assertIn("READER_BODY_CONTEXT", hal_body)
        self.assertIn("return HalDisplay::FAST_REFRESH;", hal_body)


if __name__ == "__main__":
    unittest.main()
