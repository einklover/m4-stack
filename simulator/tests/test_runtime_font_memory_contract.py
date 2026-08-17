#!/usr/bin/env python3
"""Memory/lifetime contracts for streamed runtime sfnt fonts.

These checks intentionally stay source-level: EpdFontLoader depends on the
Arduino renderer and SD stack, while the invariant we need to protect is much
simpler — every supported runtime sfnt extension must take the single active
reader-face path instead of the legacy six-size epdfont path.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "firmware/lib/EpdFontLoader/EpdFontLoader.cpp"


def source() -> str:
    return LOADER.read_text(encoding="utf-8")


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


class RuntimeSfntMemoryContracts(unittest.TestCase):
    def test_all_runtime_sfnt_extensions_use_runtime_path(self) -> None:
        body = function_body(source(), "bool isRuntimeTtfFamily(")
        for suffix in (".ttf", ".ttc", ".otf", ".otc"):
            self.assertIn(f'suffix == "{suffix}"', body)

    def test_runtime_path_creates_only_selected_reader_size(self) -> None:
        body = function_body(source(), "bool EpdFontLoader::loadFontsFromSd(")
        self.assertRegex(
            body,
            re.compile(
                r"if\s*\(runtimeTtf\)\s*\{\s*"
                r"sizes\.push_back\(runtimeReaderSize\);\s*"
                r"\}\s*else\s*\{",
                re.S,
            ),
        )

    def test_runtime_switch_releases_old_faces_before_reloading(self) -> None:
        body = function_body(source(), "bool EpdFontLoader::loadFontsFromSd(")
        release = body.index("FontManager::getInstance().releaseRuntimeTtfFaces();")
        load_loop = body.index("for (int sz : sizes)")
        self.assertLess(release, load_loop)


if __name__ == "__main__":
    unittest.main()
