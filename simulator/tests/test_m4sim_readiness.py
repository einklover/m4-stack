#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))

import m4sim  # noqa: E402


class _RunningQemu:
    returncode = None

    def poll(self):
        return None


class M4SimReadinessTests(unittest.TestCase):
    def test_bridge_only_ping_is_not_application_ready(self):
        bridge_only = json.dumps(
            {
                "protocol": 1,
                "firmware": "",
                "activity": "",
                "screen_w": 0,
                "screen_h": 0,
            }
        )
        with tempfile.TemporaryDirectory() as td:
            art = Path(td)
            qemu_log = art / "qemu.log"
            qemu_log.write_text("")
            with (
                mock.patch.object(m4sim, "ART", art),
                mock.patch.object(m4sim, "m4adb_once", return_value=(0, bridge_only)),
                mock.patch.object(m4sim.time, "time", side_effect=[0.0, 0.0, 2.0]),
                mock.patch.object(m4sim.time, "sleep"),
            ):
                with self.assertRaises(m4sim.M4SimError):
                    m4sim.wait_m4adb_ready(
                        "/dev/fake", _RunningQemu(), seconds=1.0, qemu_log=qemu_log
                    )

    def test_initialized_ping_is_application_ready(self):
        initialized = json.dumps(
            {
                "protocol": 1,
                "firmware": "202608187-murphy-m4-qemu-plugin",
                "activity": "Home",
                "screen_w": 480,
                "screen_h": 800,
            }
        )
        with tempfile.TemporaryDirectory() as td:
            art = Path(td)
            qemu_log = art / "qemu.log"
            qemu_log.write_text("")
            with (
                mock.patch.object(m4sim, "ART", art),
                mock.patch.object(m4sim, "m4adb_once", return_value=(0, initialized)),
                mock.patch.object(m4sim.time, "time", side_effect=[0.0, 0.0]),
            ):
                result = m4sim.wait_m4adb_ready(
                    "/dev/fake", _RunningQemu(), seconds=1.0, qemu_log=qemu_log
                )
        self.assertEqual(result["activity"], "Home")


if __name__ == "__main__":
    unittest.main()
