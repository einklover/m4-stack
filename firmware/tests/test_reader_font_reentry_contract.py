#!/usr/bin/env python3
"""Guards hardware-reported regression: A+/A- no-op and custom TTF lost on reopen."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
txt = (r / "src/activities/reader/TxtReaderActivity.cpp").read_text()
txth = (r / "src/activities/reader/TxtReaderActivity.h").read_text()
epub = (r / "src/activities/reader/EpubReaderActivity.cpp").read_text()
epubh = (r / "src/activities/reader/EpubReaderActivity.h").read_text()
loader = (r / "lib/EpdFontLoader/EpdFontLoader.cpp").read_text()
for name, src, end in [
    ("TXT", txt, "void TxtReaderActivity::onExit()"),
    ("EPUB", epub, "void EpubReaderActivity::onExit()"),
]:
    enter = src.split("::onEnter() {", 1)[1].split(end, 1)[0]
    assert "EpdFontLoader::ensureFontsFromSd(renderer);" in enter, name
    task_start = "M4Psram::createTask(" if name == "TXT" else "xTaskCreate("
    assert enter.index("EpdFontLoader::ensureFontsFromSd(renderer)") < enter.index(task_start), name
assert "sdFontsLoaded_ = false;" in loader.split("void EpdFontLoader::releaseRuntimeReaderFonts", 1)[1].split("void EpdFontLoader::ensureFontsFromSd", 1)[0]
ensure = loader.split("void EpdFontLoader::ensureFontsFromSd", 1)[1].split("bool EpdFontLoader::loadFontsFromSd", 1)[0]
assert "getBestFontId" in ensure, "must retry missing custom face on re-entry"
menu_enter = txt.split("void TxtReaderActivity::openMenu(",1)[1].split("void TxtReaderActivity::enterChapterPicker",1)[0]
assert "deferredMenuNeedRebuild_ = false;" in menu_enter.split("suppressDisplay_ = true",1)[0]
assert "deferredMenuNeedRebuild_ = false;" not in menu_enter.split("deferredMenuOrientation_ = newOrientation",1)[1]
assert "if (subActivity)" in txth.split("void onReaderMenuStyleChanged() override",1)[1].split("// Parent observes",1)[0]
assert "pendingSettingsRebuild_" in txt.split("void TxtReaderActivity::onSettingsChanged()",1)[1]
assert "if (!subActivity && pendingSettingsRebuild_)" in txt
assert "if (subActivity) return;" in epubh.split("void onReaderMenuStyleChanged() override",1)[1]
print("Real-device font re-entry and reflow source contracts: PASS")
