from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATCH_DIR = ROOT / "qemu" / "patches"


def series_names() -> list[str]:
    return [
        line.strip()
        for line in (PATCH_DIR / "series").read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


class QemuPatchsetTests(unittest.TestCase):
    def test_series_is_complete_and_unique(self):
        names = series_names()
        self.assertEqual(len(names), len(set(names)))
        self.assertGreaterEqual(len(names), 3)
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

    def test_gpio_patch_exposes_pin_lines_and_interrupt_connection(self):
        self.assertIn("0003-esp32-gpio-digital-io.diff", series_names())
        text = (PATCH_DIR / "0003-esp32-gpio-digital-io.diff").read_text(encoding="utf-8")
        self.assertIn('"gpio-in", ESP32_GPIO_PIN_COUNT', text)
        self.assertIn('"gpio-out", ESP32_GPIO_PIN_COUNT', text)
        self.assertIn('DEFINE_PROP_UINT64("input-default"', text)
        self.assertIn("ETS_GPIO_INTR_SOURCE", text)
        self.assertIn("GPIO_STATUS_W1TC_OFF", text)


if __name__ == "__main__":
    unittest.main()
