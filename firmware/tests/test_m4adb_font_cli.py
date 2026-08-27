#!/usr/bin/env python3
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import m4adb  # noqa: E402
from m4adb_lib.client import Client  # noqa: E402
from m4adb_lib.font_policy import font_filename_error  # noqa: E402


class RecordingTransport:
    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(data)

    def read(self, timeout=0.05):
        return ""

    def close(self):
        pass


class FontCliContract(unittest.TestCase):
    def test_parser_exposes_list_get_set(self):
        parser = m4adb.build_parser()
        self.assertEqual(parser.parse_args(["font", "list"]).font_action, "list")
        self.assertEqual(parser.parse_args(["font", "get"]).font_action, "get")
        self.assertEqual(parser.parse_args(["font", "set", "中文 字体.otf"]).filename,
                         "中文 字体.otf")

    def test_client_sends_structured_font_requests(self):
        transport = RecordingTransport()
        client = Client(transport)
        client.request = lambda obj, **kwargs: obj
        self.assertEqual(client.font_list(), {"op": "font", "action": "list"})
        self.assertEqual(client.font_get(), {"op": "font", "action": "get"})
        self.assertEqual(client.font_set("中文 字体.otf"),
                         {"op": "font", "action": "set", "filename": "中文 字体.otf"})

    def test_filename_policy_rejects_traversal_and_dotfiles(self):
        self.assertEqual(font_filename_error("中文 字体.otf"), "")
        self.assertNotEqual(font_filename_error("../font.ttf"), "")
        self.assertNotEqual(font_filename_error("/FONT/font.ttf"), "")
        self.assertNotEqual(font_filename_error(".hidden.ttf"), "")

    def test_set_failure_keeps_structured_result_and_nonzero_cli(self):
        self.assertEqual(
            {"ok": False, "filename": "bad.ttf", "stage": "load", "error": "cmap"}["stage"],
            "load",
        )
        proc = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "m4adb.py"), "--mock", "font", "set", "../bad.ttf"],
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertTrue(proc.stdout or proc.stderr)


if __name__ == "__main__":
    unittest.main()
