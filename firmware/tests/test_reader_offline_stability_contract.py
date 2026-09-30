#!/usr/bin/env python3
"""Source guards for the offline M4 real-device font trial."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
epub = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text()
loader = (root / "lib/EpdFontLoader/EpdFontLoader.cpp").read_text()
cache = (root / "lib/EpdFont/TtfGlyphSdCache.cpp").read_text()
build = (root / "platformio.ini").read_text()
# Parent enterReaderMenu takes the renderer lock itself. No callsite may
# hold this non-recursive FreeRTOS mutex a second time around entry.
entry = epub.split("void EpubReaderActivity::enterReaderMenu(", 1)[1].split(
    "void EpubReaderActivity::enterChapterSelector", 1)[0]
assert "xSemaphoreTake(renderingMutex, portMAX_DELAY)" in entry
assert "xSemaphoreGive(renderingMutex)" in entry
assert "xSemaphoreTake(renderingMutex, portMAX_DELAY);\n      exitActivity();\n      enterReaderMenu" not in epub
assert "xSemaphoreTake(renderingMutex, portMAX_DELAY);\n        exitActivity();\n        enterReaderMenu" not in epub
# Reader font destruction happens after menu task teardown.
assert "pendingMenuClose_.store(true)" in epub
assert "if (subActivity || pendingMenuClose_.load())" in epub
assert "releaseRuntimeTtfFaces(FontManager::TtfFaceRole::Reader)" in loader
# Disable only SD glyph persistence in the trial profile, never SD TTF streams.
trial = build.split("[env:murphy_m4_nosd_fontcache]", 1)[1].split("[env:", 1)[0]
assert "-DM4_SD_GLYPH_CACHE_ENABLED=0" in trial
assert "M4_SD_GLYPH_CACHE_ENABLED" in cache
print("Offline font stability contracts: PASS")
