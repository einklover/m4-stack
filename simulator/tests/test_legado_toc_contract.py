#!/usr/bin/env python3
"""Source contracts for Legado TOC stale/count/file consistency.

Locks the merged OX catalog path (policy helpers are covered by the host C++
test). The production bugs that survived two REJECT rounds were all here:
  - count cached TOC on the absolute finalPath (no double-prefixed abs path)
  - leftover placeholder toc_rows.txt is deleted before legado_shelf_stale
  - cached-TOC refill keeps the file on transient fail (no delete)
  - stale remap is Legado-only
  - empty 2xx (http_2xx_empty) classifies as a stale shelf after Fanqie
Host/static only: no QEMU, ADB, hardware, or live Legado service.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "firmware/src/apps/providers/M4NativeProviderCatalog.cpp"
POLICY = ROOT / "firmware/src/apps/providers/M4LegadoTocPolicy.h"
ACTIVITY = ROOT / "firmware/src/activities/apps/NativeProviderBookActivity.cpp"


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


class LegadoTocProductionContracts(unittest.TestCase):
    def test_cached_toc_count_uses_absolute_final_path(self) -> None:
        src = CATALOG.read_text(encoding="utf-8")
        task = function_body(src, "void taskMain(void*)")
        self.assertIn('const std::string finalPath = appRoot(job.appId) + "/" + tocRelPath(job.bookId);', task)
        self.assertIn("const size_t cachedRows = countTocRows(finalPath);", task)
        self.assertNotIn("finalAbsPath", src)
        self.assertNotIn('appRoot(job.appId) + "/" + finalPath', src)
        count = function_body(src, "size_t countTocRows(const std::string& absPath)")
        self.assertIn("SdMan.openFileForRead", count)
        self.assertIn("absPath.c_str()", count)

    def test_stale_placeholder_is_removed_before_error(self) -> None:
        src = CATALOG.read_text(encoding="utf-8")
        task = function_body(src, "void taskMain(void*)")
        stale = task[task.index("OpenStrategy::PlaceholderThenFull") :]
        self.assertIn('job.providerId == "legado"', stale)
        self.assertIn("M4LegadoTocPolicy::isStaleShelfFetch(fullTransferOk, fullErr, fullRows)", stale)
        remove = stale.index("SdMan.remove(finalPath.c_str())")
        publish = stale.index('publish(Phase::Error, 0, 0, "legado_shelf_stale")')
        self.assertLess(remove, publish, "leftover skeleton would poison the cache guard on retry")

    def test_cached_toc_refill_keeps_file_on_fail(self) -> None:
        src = CATALOG.read_text(encoding="utf-8")
        task = function_body(src, "void taskMain(void*)")
        cached = task[
            task.index("legado cached TOC wins") : task.index("OpenStrategy::PlaceholderThenFull")
        ]
        self.assertIn("keepPartialOnFail=*/true", cached)
        self.assertNotIn("SdMan.remove", cached)
        self.assertNotIn("legado_shelf_stale", cached)

    def test_empty_2xx_is_stale_shelf_and_mapped_in_ui(self) -> None:
        policy = POLICY.read_text(encoding="utf-8")
        activity = ACTIVITY.read_text(encoding="utf-8")
        stale = function_body(policy, "inline bool isStaleShelfFetch(")
        catalog = function_body(activity, "std::string catalogErrorText(")
        chapter = function_body(activity, "std::string chapterErrorText(")
        self.assertIn('error == "http_404" || error == "http_2xx_empty"', stale)
        self.assertIn('code == "http_2xx_empty"', chapter)
        self.assertIn('return "内容源返回空数据，请重试"', chapter)
        self.assertIn('code == "http_2xx_empty"', catalog)
        self.assertIn('return "目录源返回空数据，请重试"', catalog)
        self.assertIn('code == "legado_shelf_stale"', catalog)


if __name__ == "__main__":
    unittest.main()
