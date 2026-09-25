#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
menu_cpp = (ROOT / "firmware/src/activities/reader/EpubReaderMenuActivity.cpp").read_text(encoding="utf-8")
menu_h = (ROOT / "firmware/src/activities/reader/EpubReaderMenuActivity.h").read_text(encoding="utf-8")
reader_cpp = (ROOT / "firmware/src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = source.find("{", start)
    assert brace >= 0, f"missing body: {signature}"
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


close_menu = body(menu_cpp, "void EpubReaderMenuActivity::closeToReader()")
assert "notifyParentStyleChanged(false)" in close_menu, (
    "closing the Reader menu must defer font reload until the parent owns the transition"
)

notify = body(menu_cpp, "void EpubReaderMenuActivity::notifyParentStyleChanged(")
assert "if (reloadFonts && fontDirty)" in notify
assert "host->onReaderMenuStyleChanged()" in notify
assert "void notifyParentStyleChanged(bool reloadFonts = true);" in menu_h

deferred_close = body(reader_cpp, "void TxtReaderActivity::applyDeferredMenuClose()")
assert "if (needRebuild) {\n    EpdFontLoader::loadFontsFromSd(renderer);" in deferred_close, (
    "parent Reader must reload fonts once, after the child menu has exited"
)

print("Reader font reload handoff contract: PASS")
