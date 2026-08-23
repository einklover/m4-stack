#!/usr/bin/env python3
"""Source contracts for the large local-TXT instant-open path."""
from __future__ import annotations

import unittest
from pathlib import Path
import re

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


class LargeTxtOpenContracts(unittest.TestCase):
    def test_large_txt_forces_direct_reader(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/ReaderActivity.cpp"),
            "void ReaderActivity::onGoToTxtReader(",
        )
        self.assertIn("kLargeTxtDirectThreshold", src)
        self.assertIn("txt->getFileSize()", src)
        self.assertIn("large_txt_auto_direct", src)

    def test_txt_conversion_progress_does_not_flash_panel(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/ReaderActivity.cpp"),
            "void ReaderActivity::onGoToTxtReader(",
        )
        self.assertNotIn("shouldRefreshProgressUi", src)

    def test_direct_txt_skips_generic_loading_screen(self) -> None:
        src = text("firmware/src/activities/reader/ReaderActivity.cpp")
        marker = "// Show loading indicator only on first open"
        section = src[src.index(marker) : src.index("currentBookPath = initialBookPath;", src.index(marker))]
        self.assertIn("!isTxtFile(initialBookPath)", section)

    def test_large_reader_indexes_before_chapter_scan(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/TxtReaderActivity.cpp"),
            "void TxtReaderActivity::chapter_initializeReader(",
        )
        self.assertIn("largeTxtFastOpen_", src)
        self.assertIn("large_txt_fast_open", src)
        self.assertLess(src.index("buildPageIndexFirstPage"), src.index("tryLoadChapterMeta"))

    def test_progress_record_keeps_legacy_fields_and_saves_byte_offset(self) -> None:
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        save = function_body(src, "void TxtReaderActivity::saveProgress() const")
        load = function_body(src, "void TxtReaderActivity::loadProgress()")
        self.assertIn("kProgressDataBytes", save)
        self.assertIn("pageOffsets", save)
        self.assertIn("resumeByte", save)
        self.assertIn("kLegacyProgressDataBytes", load)
        self.assertIn("pendingRestoreByte_", load)

    def test_render_does_not_sync_progress_to_sd(self) -> None:
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        render = function_body(src, "void TxtReaderActivity::renderScreen()")
        self.assertNotIn("saveProgress();", render)
        self.assertIn("progressSavePending_", src)

    def test_open_history_persistence_is_deferred_until_after_first_paint(self) -> None:
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        header = text("firmware/src/activities/reader/TxtReaderActivity.h")
        enter = function_body(src, "void TxtReaderActivity::onEnter()")
        self.assertIn("persistOpenHistory", header)
        self.assertIn("openHistorySavePending_", header)
        self.assertNotIn("APP_STATE.saveToFile();", enter)
        self.assertNotIn("RECENT_BOOKS.addBook", enter)
        self.assertIn("persistOpenHistory();", src)
        self.assertIn("first_physical_done kind=library", src)

    def test_page_turns_wait_for_first_physical_frame(self) -> None:
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        body = function_body(src, "void TxtReaderActivity::pageTurnLocked(")
        self.assertIn("!firstPhysicalShown_.load", body)
        self.assertIn("pendingTurnDelta_.store(0", body)

    def test_chapter_discovery_runs_in_background_batches(self) -> None:
        hdr = text("firmware/src/activities/reader/TxtReaderActivity.h")
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        self.assertIn("libraryIdleDiscoverChapterBatch", hdr)
        self.assertIn("libraryIdleDiscoverChapterBatch", src)
        self.assertIn("chapterDiscoveryBatch_", hdr)

    def test_high_density_chapter_discovery_waits_for_physical_page(self) -> None:
        hdr = text("firmware/src/activities/reader/TxtReaderActivity.h")
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        discover = function_body(src, "void TxtReaderActivity::libraryIdleDiscoverChapterBatch(")
        self.assertIn("firstPhysicalShown_.load", discover)
        self.assertIn("chapterDiscoveryNotBeforeMs_", discover)
        self.assertIn("chapterDiscoveryNextMs_", discover)
        self.assertIn("chapterDiscoveryNotBeforeMs_", hdr)
        self.assertIn("chapterDiscoveryNextMs_", hdr)

    def test_pathological_high_density_fixture_is_deterministic(self) -> None:
        src = text("simulator/tests/large_txt_first_open_e2e.py")
        self.assertIn("HIGH_DENSITY_FIXTURE_NAME", src)
        self.assertIn("HIGH_DENSITY_CHAPTERS = 40416", src)
        self.assertIn("--high-chapter-density", src)
        self.assertIn("chapter_discovery_before_first_physical", src)

    def test_refresh_policy_has_explicit_reader_cleanup_context(self) -> None:
        hal = text("firmware/lib/hal/HalDisplay.h")
        gfx = text("firmware/lib/GfxRenderer/GfxRenderer.h")
        freeink = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h")
        self.assertIn("READER_CLEANUP_REFRESH", hal)
        self.assertIn("RefreshContext", hal)
        self.assertIn("READER_BODY_CONTEXT", hal)
        self.assertIn("READER_BODY_CONTEXT", gfx)
        # The generated SDK is ignored; policy-aware copies expose the
        # extension, while a pinned legacy copy safely falls back to FAST.
        if freeink and "READER_CLEANUP_REFRESH" in freeink:
            self.assertIn("READER_CLEANUP_REFRESH", freeink)

    def test_renderer_does_not_auto_promote_ui_to_full(self) -> None:
        src = text("firmware/lib/GfxRenderer/GfxRenderer.cpp")
        body = function_body(src, "void GfxRenderer::displayBuffer(")
        self.assertIn("READER_BODY_CONTEXT", body)
        self.assertNotIn("auto-promote FAST->FULL", body)
        self.assertNotIn("partialsSinceFull_", body)

    def test_legacy_full_api_is_guarded_and_cleanup_is_single_pass(self) -> None:
        freeink_header = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h")
        if "READER_CLEANUP_REFRESH" not in freeink_header:
            self.skipTest("generated SDK is the legacy fast-only copy")
        facade = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp")
        driver = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp")
        self.assertIn("case FreeInkDisplay::FULL_REFRESH: return RefreshMode::Fast", facade)
        self.assertIn("case FreeInkDisplay::HALF_REFRESH: return RefreshMode::Fast", facade)
        refresh = function_body(driver, "void Ssd1677Driver::refresh(")
        self.assertIn("RefreshMode::ReaderCleanup", refresh)
        self.assertNotIn("_cfg.fullSeqOverride", refresh[refresh.index("const uint8_t seqOverride"):])
        impl = function_body(driver, "void Ssd1677Driver::displayImpl(")
        self.assertNotIn("mode = RefreshMode::Full", impl)
        self.assertNotIn("mode = RefreshMode::Half", impl)

    def test_driver_abstraction_blocks_legacy_waveform_escape_hatches(self) -> None:
        hal = text("firmware/lib/hal/HalDisplay.cpp")
        freeink_header = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h")
        facade = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp")
        lab = text("firmware/src/debug/M4WaveformLab.cpp")
        ed2208 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Ed2208M5Driver.cpp")
        ed2208_header = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Ed2208M5Driver.h")
        x3 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp")
        it8951 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/It8951Driver.cpp")

        # The explicit reader page-turn path is the one deliberate custom-LUT
        # exception. It is windowed and ends with a stock same-frame seed;
        # generic displayBuffer/refreshDisplay policy remains FAST-only.
        self.assertIn("setCustomLut(_bus, true", facade)
        self.assertIn("setCustomLUT(bool enabled", facade)
        self.assertIn("einkDisplay.waveformLabRefresh(prev, next, lut, turnOff)", hal)
        self.assertIn("einkDisplay.waveformLabRefreshWindow(prev, next, lut", hal)
        self.assertIn("einkDisplay.waveformLabRefreshWindowBufs(redWin, bwWin, lut", hal)
        self.assertIn("einkDisplay.waveformLabActivate(lut)", hal)
        self.assertIn("einkDisplay.waveformLabActivateWindow(x, y, w, h, lut)", hal)
        self.assertIn("einkDisplay.setCustomLUT(enabled, lutData)", hal)
        self.assertIn("waveformLabRefresh(frame, frame, nullptr, false)", hal)
        animation = function_body(lab, "uint32_t runAnimateMemWindow(")
        self.assertIn("setCustomLUT(true, gLut)", animation)
        self.assertIn("waveformLabActivate(gLut)", animation)
        self.assertIn("setCustomLUT(false, nullptr)", animation)
        self.assertIn("waveformLabRefresh(newFrame, newFrame, /*lut=*/nullptr", animation)
        self.assertNotRegex(animation, r"displayBuffer\s*\([^;]*(?:FULL_REFRESH|HALF_REFRESH)")

        if "READER_CLEANUP_REFRESH" not in freeink_header:
            self.skipTest("generated SDK is the legacy fast-only copy")

        # ED2208's old 15-second OTP path must be unreachable even if a legacy
        # caller reaches the concrete driver rather than the facade.
        self.assertIn("requestCompleteWaveformNextRefresh() override {}", ed2208_header)
        self.assertNotIn("_completeNextRefresh", ed2208)
        self.assertNotIn("completeWaveform", ed2208)

        # Grayscale/rebase helpers are not allowed to select the old full bank.
        self.assertNotIn("_cfg.full", x3[x3.index("bool Uc8253X3Driver::displayStart"):])
        self.assertNotIn("_cfg.fullMode", it8951[it8951.index("void It8951Driver::displayGray"):])

    def test_simulator_refresh_contract_normalizes_legacy_modes(self) -> None:
        display_port = text("simulator/platform/DisplayPort.h")
        panel = text("simulator/hardware/SimPanel.h")
        self.assertIn("normalizeRefreshMode", display_port)
        self.assertIn("mode_ = m4platform::normalizeRefreshMode(mode, context)", panel)
        self.assertIn("case RefreshMode::READER_CLEANUP_REFRESH: return 1", panel)
        self.assertNotIn("case RefreshMode::FULL_REFRESH: return 2", panel)
        self.assertNotIn("case RefreshMode::HALF_REFRESH:\n      case RefreshMode::READER_CLEANUP_REFRESH: return 1", panel)

    def test_grayscale_and_waveform_lab_paths_are_fast_only(self) -> None:
        freeink_header = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h")
        if "READER_CLEANUP_REFRESH" not in freeink_header:
            self.skipTest("generated SDK is the legacy fast-only copy")
        x3 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp")
        ed2208 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Ed2208M5Driver.cpp")
        it8951 = text("firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/It8951Driver.cpp")
        self.assertIn("loadBankCdi(bus, 0x29, 0x07, _cfg.gc)", x3)
        self.assertIn("loadBankCdi(bus, 0x29, 0x07, _cfg.fast)", x3)
        self.assertIn("const uint16_t gmode = _cfg.grayMode", it8951)
        self.assertIn("interruptRefresh(bus)", ed2208)

    def test_non_reader_refresh_call_sites_do_not_request_legacy_strong_modes(self) -> None:
        paths = (
            "firmware/src/main.cpp",
            "firmware/src/debug/M4WaveformLab.cpp",
            "firmware/src/activities/home/HomeActivity.cpp",
            "firmware/src/activities/home/MyLibraryActivity.cpp",
            "firmware/src/activities/reader/EpubReaderMenuActivity.cpp",
            "firmware/src/activities/reader/EpubReaderSettingsActivity.cpp",
            "firmware/src/activities/reader/BookmarkManagerActivity.cpp",
            "firmware/src/util/M4ErrorScreen.h",
        )
        for rel in paths:
            src = text(rel)
            self.assertNotRegex(
                src,
                r"(?:displayBuffer|refreshDisplay)\([^;]*(?:FULL_REFRESH|HALF_REFRESH)",
                msg=rel,
            )

    def test_all_tracked_refresh_call_sites_use_policy_safe_modes(self) -> None:
        root = ROOT / "firmware"
        call = re.compile(r"(?:displayBuffer|refreshDisplay)\s*\([^;]*?(?:FULL_REFRESH|HALF_REFRESH)[^;]*\)", re.S)
        for rel in sorted(root.rglob("*")):
            if not rel.is_file() or rel.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            if "open-m4-sdk" in rel.parts or ".pio" in rel.parts:
                continue
            self.assertIsNone(call.search(rel.read_text(encoding="utf-8", errors="replace")), msg=str(rel))

    def test_reader_cleanup_is_only_used_by_reader_body_paths(self) -> None:
        reader_paths = (
            "firmware/src/activities/reader/TxtReaderActivity.cpp",
            "firmware/src/activities/reader/EpubReaderActivity.cpp",
            "firmware/src/activities/reader/XtcReaderActivity.cpp",
        )
        for rel in reader_paths:
            self.assertIn("READER_CLEANUP_REFRESH", text(rel), msg=rel)
        for rel in (
            "firmware/src/main.cpp",
            "firmware/src/activities/home/HomeActivity.cpp",
            "firmware/src/activities/home/MyLibraryActivity.cpp",
            "firmware/src/activities/boot_sleep/SleepActivity.cpp",
        ):
            self.assertNotIn("READER_CLEANUP_REFRESH", text(rel), msg=rel)

    def test_chapter_picker_paint_is_cache_only(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/TxtReaderChapterSelectionActivity.cpp"),
            "void TxtReaderChapterSelectionActivity::materializePageTitles(",
        )
        self.assertIn("parseChapterIndexAndOffset(batch, /*allowScan=*/false)", src)

    def test_chapter_picker_idle_never_scans_or_restores_batches(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/TxtReaderChapterSelectionActivity.cpp"),
            "void TxtReaderChapterSelectionActivity::displayTaskLoop(",
        )
        self.assertNotIn("prefetchNextBatchQuiet", src)
        self.assertNotIn("allowScan=*/true", src)


if __name__ == "__main__":
    unittest.main()
