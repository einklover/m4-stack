#!/usr/bin/env python3
"""Regression test for serial logs coalesced behind an m4adb reply."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))

from m4adb_lib.protocol import PREFIX, encode_json_b64  # noqa: E402
from m4adb_lib.transport import Transport  # noqa: E402
from m4adb_observing_client import ObservingClient  # noqa: E402


MARKER = "Entering activity: NetworkModeSelection"


class CoalescedReplyTransport(Transport):
    """Return matching OK + ordinary firmware log in one OS-style read."""

    def __init__(self) -> None:
        self._rx = ""

    def write(self, data: str) -> None:
        rid = data.strip().split()[1]
        payload = encode_json_b64({"op": "status", "activity": "CrossPointWebServer"})
        self._rx = f"{PREFIX} {rid} ok {payload}\n{MARKER}\n"

    def read(self, timeout: float = 0.05) -> str:
        out = self._rx
        self._rx = ""
        return out

    def close(self) -> None:
        pass


class ObservingClientTests(unittest.TestCase):
    def test_preserves_log_after_matching_reply_in_same_read(self) -> None:
        client = ObservingClient(CoalescedReplyTransport(), default_timeout=0.5)
        result = client.status()
        self.assertEqual(result["activity"], "CrossPointWebServer")
        self.assertIn(MARKER, client.serial_log)


if __name__ == "__main__":
    unittest.main()
