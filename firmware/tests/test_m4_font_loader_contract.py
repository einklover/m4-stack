#!/usr/bin/env python3
"""Regression contract for runtime custom-font insertion success handling."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "firmware" / "lib" / "EpdFontLoader" / "EpdFontLoader.cpp"


def test_signed_font_id_is_not_used_as_load_success():
    source = LOADER.read_text(encoding="utf-8")
    assert "bool loadAndInsertCustom(" in source
    assert "any = loadAndInsertCustom(renderer, d.loadCustomFamily.c_str(), sz, loadedCustomIds) || any;" in source
    assert "loadAndInsertCustom(renderer, d.loadCustomFamily.c_str(), sz, loadedCustomIds) >= 0" not in source
    assert "loadAndInsertCustom(renderer, SETTINGS.customFontFamily, size, loadedCustomIds);" in source


if __name__ == "__main__":
    test_signed_font_id_is_not_used_as_load_success()
    print("m4 font loader contract passed")
