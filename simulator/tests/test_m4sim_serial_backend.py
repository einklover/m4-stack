#!/usr/bin/env python3
"""m4sim serial backend selection: PTY default, pipe only when PTY unavailable."""
from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))

import m4sim  # noqa: E402


class SerialBackendSelectionTests(unittest.TestCase):
    def tearDown(self) -> None:
        os.environ.pop("M4SIM_SERIAL", None)

    def test_default_prefers_pty_when_openpty_works(self):
        os.environ.pop("M4SIM_SERIAL", None)
        # This host can allocate PTYs (CI/dev machines). Sandbox-only hosts
        # that deny ptmx are covered by the forced-pipe test below.
        if not m4sim.can_use_pty():
            self.skipTest("host cannot allocate PTY; cannot assert default")
        mode, pipe_base = m4sim.resolve_serial_backend(Path("/tmp"))
        self.assertEqual(mode, "pty")
        self.assertIsNone(pipe_base)

    def test_env_force_pipe_overrides_available_pty(self):
        os.environ["M4SIM_SERIAL"] = "pipe"
        with tempfile.TemporaryDirectory() as td:
            art = Path(td)
            mode, pipe_base = m4sim.resolve_serial_backend(art)
            self.assertEqual(mode, "pipe")
            self.assertEqual(pipe_base, str(art / "m4uart.pipe"))

    def test_env_force_pty_overrides(self):
        os.environ["M4SIM_SERIAL"] = "pty"
        mode, pipe_base = m4sim.resolve_serial_backend(Path("/tmp"))
        self.assertEqual(mode, "pty")
        self.assertIsNone(pipe_base)

    def test_can_use_pty_respects_force_pipe(self):
        os.environ["M4SIM_SERIAL"] = "pipe"
        self.assertFalse(m4sim.can_use_pty())


if __name__ == "__main__":
    unittest.main()
