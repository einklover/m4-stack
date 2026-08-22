#!/usr/bin/env python3
"""Source contracts for the large local-TXT instant-open path."""
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

    def test_chapter_discovery_runs_in_background_batches(self) -> None:
        hdr = text("firmware/src/activities/reader/TxtReaderActivity.h")
        src = text("firmware/src/activities/reader/TxtReaderActivity.cpp")
        self.assertIn("libraryIdleDiscoverChapterBatch", hdr)
        self.assertIn("libraryIdleDiscoverChapterBatch", src)
        self.assertIn("chapterDiscoveryBatch_", hdr)

    def test_chapter_picker_paint_is_cache_only(self) -> None:
        src = function_body(
            text("firmware/src/activities/reader/TxtReaderChapterSelectionActivity.cpp"),
            "void TxtReaderChapterSelectionActivity::materializePageTitles(",
        )
        self.assertIn("parseChapterIndexAndOffset(batch, /*allowScan=*/false)", src)


if __name__ == "__main__":
    unittest.main()
