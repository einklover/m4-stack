#!/usr/bin/env python3
"""Cheap source contracts for Stage 14 real-device regressions.

These are intentionally narrow. They protect architectural invariants whose
violations previously compiled successfully but caused callback-stack UAF or
large-catalog false timeouts on device.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


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


class NetworkLifecycleContracts(unittest.TestCase):
    def assert_safe_child_pump(self, rel: str, signature: str) -> None:
        body = function_body(text(rel), signature)
        self.assertIn("pumpSubActivityFrame();", body)
        self.assertNotIn("subActivity->loop()", body)

    def test_transfer_activity_never_pumps_child_directly(self) -> None:
        self.assert_safe_child_pump(
            "firmware/src/activities/network/CrossPointWebServerActivity.cpp",
            "void CrossPointWebServerActivity::loop()",
        )

    def test_calibre_activity_never_pumps_child_directly(self) -> None:
        self.assert_safe_child_pump(
            "firmware/src/activities/network/CalibreConnectActivity.cpp",
            "void CalibreConnectActivity::loop()",
        )

    def test_network_setup_failures_stay_inside_network_ui(self) -> None:
        src = text("firmware/src/activities/network/CrossPointWebServerActivity.cpp")
        ap = function_body(src, "void CrossPointWebServerActivity::startAccessPoint()")
        server = function_body(src, "void CrossPointWebServerActivity::startWebServer()")
        self.assertIn('showSetupError("Hotspot startup failed")', ap)
        self.assertNotIn("onGoBack();", ap)
        self.assertRegex(server, r"showSetupError\(\"Web server (memory allocation|startup) failed\"\)")
        self.assertNotIn("onGoBack();", server)


class ProgressiveLoaderContracts(unittest.TestCase):
    def test_timeout_is_renewed_after_http_connect(self) -> None:
        src = text("firmware/src/apps/M4xProgressiveLoader.cpp")
        body = function_body(src, "bool Session::connectHttp(")
        self.assertIn("startMs_ = nowMs();", body)

    def test_timeout_is_renewed_for_each_payload_progress(self) -> None:
        src = text("firmware/src/apps/M4xProgressiveLoader.cpp")
        body = function_body(src, "bool Session::acceptPayload(")
        anchor = body.index("startMs_ = nowMs();")
        size_guard = body.index("if (bytes_ + len > maxBytes_)")
        self.assertGreater(anchor, size_guard)

    def test_timeout_still_uses_wrap_safe_unsigned_delta(self) -> None:
        src = text("firmware/src/apps/M4xProgressiveLoader.cpp")
        bodies = [
            function_body(src, "size_t Session::readDecoded("),
            function_body(src, "bool Session::pump("),
        ]
        for body in bodies:
            self.assertIn("static_cast<uint32_t>(nowMs() - startMs_) >= timeoutMs_", body)

    def test_header_documents_inactivity_semantics(self) -> None:
        hdr = text("firmware/src/apps/M4xProgressiveLoader.h")
        self.assertGreaterEqual(hdr.count("no accepted payload progress"), 2)
        self.assertIn("Inactivity anchor", hdr)


if __name__ == "__main__":
    unittest.main()
