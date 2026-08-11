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
        self.assertGreaterEqual(len(names), 7)
        for name in names:
            self.assertTrue((PATCH_DIR / name).is_file(), name)

    def test_upstream_is_commit_pinned(self):
        data = json.loads((PATCH_DIR / "upstream.json").read_text(encoding="utf-8"))
        ref = data["ref"]
        self.assertEqual(len(ref), 40)
        self.assertTrue(all(ch in "0123456789abcdef" for ch in ref))

    def test_spi1_fix_uses_loop_index_for_both_bounds(self):
        text = (PATCH_DIR / "0002-esp32s3-spi-transfer-index.diff").read_text(encoding="utf-8")
        self.assertIn("+        if (i < tx_bytes) {", text)
        self.assertIn("+        if (i < rx_bytes) {", text)

    def test_gpio_is_context_checked_transform(self):
        self.assertIn("0003-esp32-gpio-digital-io.py", series_names())
        text = (PATCH_DIR / "0003-esp32-gpio-digital-io.py").read_text(encoding="utf-8")
        self.assertIn("gpio-in", text)
        self.assertIn("gpio-out", text)
        self.assertIn("ETS_GPIO_INTR_SOURCE", text)

    def test_i2c_and_murphy_machine_are_ordered_before_gpspi(self):
        names = series_names()
        self.assertLess(names.index("0004-esp32s3-i2c-controller.py"),
                        names.index("0005-murphy-m4-machine-touch.py"))
        self.assertLess(names.index("0005-murphy-m4-machine-touch.py"),
                        names.index("0007-esp32s3-gpspi2.py"))

    def test_gpspi2_transform_has_s3_mmio_irq_and_ssi_bus(self):
        text = (PATCH_DIR / "0007-esp32s3-gpspi2.py").read_text(encoding="utf-8")
        self.assertIn("DR_REG_SPI2_BASE", text)
        self.assertIn("ETS_SPI2_INTR_SOURCE", text)
        self.assertIn('ssi_create_bus(dev, "spi")', text)
        self.assertIn("R_MS_DLEN", text)
        self.assertIn("CMD_USR", text)
        self.assertIn("INT_TRANS_DONE", text)


if __name__ == "__main__":
    unittest.main()
