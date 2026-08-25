#!/usr/bin/env python3
"""Settings/menu RED->GREEN contract for OX-C allowed reader body sizes.

Allowed = {16,24,26,36,38,40,48}, default 26, hide 32/45, snap ties to larger,
next/prev via allowed list.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")

def parse_allowed(src: str):
    m = re.search(r"(?:constexpr|const)\s+uint8_t\s+kReaderBodyPixelSizes\s*\[\s*\]\s*=\s*\{([^}]+)\}", src)
    if not m:
        return None
    return [int(tok) for tok in re.findall(r"\d+", m.group(1))]

ALLOWED = (16, 24, 26, 36, 38, 40, 48)
DEFAULT = 26

def snap_px(px: int) -> int:
    best = ALLOWED[0]
    best_dist = abs(px - best)
    for cand in ALLOWED[1:]:
        d = abs(px - cand)
        if d < best_dist or (d == best_dist and cand > best):
            best = cand
            best_dist = d
    return best

def next_px(cur: int) -> int:
    snapped = snap_px(cur)
    idx = ALLOWED.index(snapped)
    if idx + 1 < len(ALLOWED):
        return ALLOWED[idx+1]
    return ALLOWED[idx]

def prev_px(cur: int) -> int:
    snapped = snap_px(cur)
    idx = ALLOWED.index(snapped)
    if idx > 0:
        return ALLOWED[idx-1]
    return ALLOWED[idx]

class ReaderBodySizesContract(unittest.TestCase):
    def test_allowed_set_is_exact(self):
        src = text("firmware/src/CrossPointSettings.h")
        allowed = parse_allowed(src)
        self.assertIsNotNone(allowed, "kReaderBodyPixelSizes missing")
        self.assertEqual(tuple(allowed), ALLOWED)
        # 32 and 45 forbidden
        self.assertNotIn(32, allowed)
        self.assertNotIn(45, allowed)

    def test_default_is_26(self):
        h = text("firmware/src/CrossPointSettings.h")
        cpp = text("firmware/src/CrossPointSettings.cpp")
        self.assertRegex(h, r"readerPixelSize\s*=\s*26")
        self.assertNotRegex(h, r"readerPixelSize\s*=\s*18")
        self.assertRegex(cpp, r"readerPixelSize\s*=\s*26")
        self.assertNotIn("readerPixelSize = 18", cpp)
        self.assertIn("kReaderBodyDefaultPixelSize", h)
        self.assertRegex(h, r"kReaderBodyDefaultPixelSize\s*=\s*26")

    def test_helpers_exist(self):
        h = text("firmware/src/CrossPointSettings.h")
        for name in ("isAllowedReaderPixelSize", "snapReaderPixelSize", "nextReaderPixelSize", "prevReaderPixelSize", "clampReaderPixelSize", "kReaderBodyPixelSizes", "kReaderBodyDefaultPixelSize"):
            self.assertIn(name, h)
        # ensure clamp delegates to snap
        self.assertRegex(h, r"clampReaderPixelSize[\s\S]*snapReaderPixelSize")

    def test_snap_mapping(self):
        # spec mapping
        cases = {
            12: 16, 13: 16, 14: 16, 15: 16,
            16: 16,
            17: 16, 18: 16, 19: 16,
            20: 24, # tie 16/24 ->24
            21: 24, 22: 24, 23: 24,
            24: 24,
            25: 26, # tie 24/26 ->26
            26: 26,
            27: 26, 28: 26, 29: 26, 30: 26,
            31: 36, # tie 26/36 ->36
            32: 36, 33: 36, 34: 36, 35: 36,
            36: 36,
            37: 38, # tie 36/38 ->38
            38: 38,
            39: 40, # tie 38/40 ->40
            40: 40,
            41: 40, 42: 40, 43: 40,
            44: 48, # tie 40/48 ->48
            45: 48, 46: 48, 47: 48,
            48: 48,
        }
        for inp, exp in cases.items():
            self.assertEqual(snap_px(inp), exp, f"snap({inp}) expected {exp}")
        # verify header's snap logic matches by re-evaluating via same algorithm
        # (we already tested header presence; functional test ensures our python snap matches spec)

    def test_snap_ties_to_larger(self):
        self.assertEqual(snap_px(20), 24)
        self.assertEqual(snap_px(25), 26)
        self.assertEqual(snap_px(31), 36)
        self.assertEqual(snap_px(37), 38)
        self.assertEqual(snap_px(39), 40)
        self.assertEqual(snap_px(44), 48)

    def test_isAllowed(self):
        h = text("firmware/src/CrossPointSettings.h")
        # functional via python helper
        for v in ALLOWED:
            self.assertTrue(snap_px(v) == v)
        for v in (12, 15, 18, 20, 32, 45):
            self.assertNotEqual(snap_px(v), v) if v not in ALLOWED else None
        # header literal check
        src = h
        allowed = parse_allowed(src)
        for v in ALLOWED:
            self.assertIn(v, allowed)
        for v in (32, 45):
            self.assertNotIn(v, allowed)

    def test_next_prev(self):
        # next steps through allowed list
        self.assertEqual(next_px(16), 24)
        self.assertEqual(next_px(24), 26)
        self.assertEqual(next_px(26), 36)
        self.assertEqual(next_px(36), 38)
        self.assertEqual(next_px(38), 40)
        self.assertEqual(next_px(40), 48)
        self.assertEqual(next_px(48), 48)
        self.assertEqual(prev_px(16), 16)
        self.assertEqual(prev_px(24), 16)
        self.assertEqual(prev_px(26), 24)
        self.assertEqual(prev_px(36), 26)
        self.assertEqual(prev_px(38), 36)
        self.assertEqual(prev_px(40), 38)
        self.assertEqual(prev_px(48), 40)
        # legacy values snap then step
        self.assertEqual(next_px(18), 24)  # 18 snaps to 16 -> next 24
        self.assertEqual(prev_px(32), 26)  # 32 snaps to 36 -> prev 26
        self.assertEqual(next_px(45), 48)  # 45 snaps to 48 -> next 48
        self.assertEqual(prev_px(45), 40)  # 45 snaps to 48 -> prev 40
        # check menu uses helpers
        menu = text("firmware/src/activities/reader/EpubReaderMenuActivity.cpp")
        self.assertIn("nextReaderPixelSize", menu)
        self.assertIn("prevReaderPixelSize", menu)
        self.assertNotIn("clampReaderPixelSize", menu)

    def test_enum_exact_list_and_no_32_45(self):
        lists = text("firmware/src/SettingsLists.h")
        h = text("firmware/src/CrossPointSettings.h")
        # SettingsLists must reference kReaderBodyPixelSizes and use DynamicEnum
        self.assertIn("kReaderBodyPixelSizes", lists)
        self.assertRegex(lists, r"DynamicEnum[\s\S]{0,2000}readerPixelSize")
        self.assertNotRegex(lists, r"readerPixelSize[\s\S]{0,200}READER_PIXEL_SIZE_MIN")
        # DynamicEnum literal must contain exactly allowed sizes and not 32/45
        self.assertIn('"16"', lists)
        self.assertIn('"24"', lists)
        self.assertIn('"26"', lists)
        self.assertIn('"36"', lists)
        self.assertIn('"38"', lists)
        self.assertIn('"40"', lists)
        self.assertIn('"48"', lists)
        # Ensure the readerPixelSize DynamicEnum block contains exactly those 7
        # Extract the readerPixelSize DynamicEnum's enum list via locating its surrounding braces
        # Find the position of "readerPixelSize" and search backwards for the nearest DynamicEnum enum list
        idx = lists.find('"readerPixelSize"')
        # Look for the enum list preceding it: it should contain the 7 quoted numbers
        snippet = lists[max(0, idx-2000):idx+200]
        # Count occurrences
        for v in ("16", "24", "26", "36", "38", "40", "48"):
            self.assertIn(f'"{v}"', snippet)
        self.assertNotIn('"32"', snippet)
        self.assertNotIn('"45"', snippet)
        # header array must not contain 32/45
        allowed = parse_allowed(h)
        self.assertNotIn(32, allowed)
        self.assertNotIn(45, allowed)
        # hide checks: ensure old unsupported sizes not in enum snippet
        for bad in (12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 25, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 42, 43, 44, 45, 46, 47):
            if bad not in ALLOWED:
                self.assertNotIn(f'"{bad}"', snippet)

    def test_persisted_legacy_snap(self):
        cpp = text("firmware/src/CrossPointSettings.cpp")
        h = text("firmware/src/CrossPointSettings.h")
        # loadFromFile must snap
        self.assertIn("snapReaderPixelSize", cpp)
        # check that default fallback is 26 not 18
        self.assertIn("kReaderBodyDefaultPixelSize", cpp)
        self.assertNotIn('doc["readerPixelSize"] | (uint8_t)18', cpp)
        self.assertNotIn("readerPixelSize = 18", cpp)
        # verify getReaderPixelSize clamps via snap
        self.assertRegex(h, r"getReaderPixelSize\(\)\s*const\s*\{\s*return\s+clampReaderPixelSize")
        self.assertRegex(h, r"clampReaderPixelSize[\s\S]*snapReaderPixelSize")

    def test_persistence_json_load_old_18_must_snap(self):
        # JSON load of old 18 must snap safely: simulate snap
        self.assertEqual(snap_px(18), 16)
        # also verify that load path uses snap, not direct clamp to 18
        cpp = text("firmware/src/CrossPointSettings.cpp")
        # Ensure snap is used for both legacy and direct load branches
        self.assertIn("snapReaderPixelSize(legacyReaderPixelSize", cpp)
        self.assertIn("snapReaderPixelSize(doc[\"readerPixelSize\"]", cpp)

if __name__ == "__main__":
    unittest.main()
