from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATCH_DIR = ROOT / "qemu" / "patches"


class QemuPatchsetTests(unittest.TestCase):
    def test_series_is_complete_and_unique(self):
        names = [
            line.strip()
            for line in (PATCH_DIR / "series").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        self.assertEqual(len(names), len(set(names)))
        self.assertGreaterEqual(len(names), 2)
        for name in names:
            self.assertTrue((PATCH_DIR / name).is_file(), name)

    def test_upstream_is_commit_pinned(self):
        data = json.loads((PATCH_DIR / "upstream.json").read_text(encoding="utf-8"))
        ref = data["ref"]
        self.assertEqual(len(ref), 40)
        self.assertTrue(all(ch in "0123456789abcdef" for ch in ref))

    def test_spi_fix_uses_loop_index_for_both_bounds(self):
        text = (PATCH_DIR / "0002-esp32s3-spi-transfer-index.diff").read_text(encoding="utf-8")
        self.assertIn("+        if (i < tx_bytes) {", text)
        self.assertIn("+        if (i < rx_bytes) {", text)
        self.assertIn("-        if (byte < tx_bytes) {", text)
        self.assertIn("-        if (byte < rx_bytes) {", text)


if __name__ == "__main__":
    unittest.main()
