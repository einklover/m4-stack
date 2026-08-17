#!/usr/bin/env python3
"""Host-side m4adb transport helpers used by ./m4sim ui."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))

from m4adb_lib.transport import is_pty_port  # noqa: E402


class PtyPortTests(unittest.TestCase):
    def test_qemu_macos_and_linux_ptys(self):
        self.assertTrue(is_pty_port("/dev/ttys046"))
        self.assertTrue(is_pty_port("/dev/pts/3"))
        self.assertTrue(is_pty_port("ttys001"))

    def test_real_usb_serial_still_drains(self):
        self.assertFalse(is_pty_port("/dev/cu.usbmodem1101"))
        self.assertFalse(is_pty_port("/dev/ttyACM0"))
        self.assertFalse(is_pty_port("/dev/ttyUSB0"))


if __name__ == "__main__":
    unittest.main()
