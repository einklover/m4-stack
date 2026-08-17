#!/usr/bin/env python3
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from paseo_agent_runner import (
    TASK_MARKER,
    parse_task,
    parse_timeout,
    ParseError,
    extract_agent_id,
    format_result,
    interpret_outcome,
    last_json_object,
)


class ParseTaskTests(unittest.TestCase):
    def test_parse_smoke_task(self) -> None:
        body = f"""{TASK_MARKER}

task_id: dispatcher-smoke-001
issue: 27
repo: einklover/m4-stack
branch: agent/eink-browser-bridge
mode: inspect-only

goal:
Verify the dispatcher.

acceptance:
- result is posted
"""
        task = parse_task(body)
        self.assertEqual(task.task_id, "dispatcher-smoke-001")
        self.assertEqual(task.issue, 27)
        self.assertEqual(task.branch, "agent/eink-browser-bridge")
        self.assertTrue(task.is_inspect_only())
        self.assertIn("Verify the dispatcher.", task.fields["goal"])

    def test_reject_missing_marker(self) -> None:
        with self.assertRaises(ParseError):
            parse_task("hello")

    def test_reject_bad_task_id(self) -> None:
        with self.assertRaises(ParseError):
            parse_task(f"{TASK_MARKER}\n\ntask_id: has space\n")

    def test_reject_other_repo(self) -> None:
        with self.assertRaises(ParseError):
            parse_task(f"{TASK_MARKER}\n\ntask_id: x\nrepo: evil/other\n")

    def test_reject_unknown_provider(self) -> None:
        with self.assertRaises(ParseError):
            parse_task(f"{TASK_MARKER}\n\ntask_id: x\nprovider: /bin/sh\n")

    def test_timeout_parse(self) -> None:
        self.assertEqual(parse_timeout("90m"), 5400)
        self.assertEqual(parse_timeout("2h"), 7200)
        self.assertEqual(parse_timeout("180m"), 10800)

    def test_extract_agent_id(self) -> None:
        self.assertEqual(
            extract_agent_id('{"Id":"2b0796e8-1080-481b-873e-eb10b3c27275"}'),
            "2b0796e8-1080-481b-873e-eb10b3c27275",
        )

    def test_format_result_marker(self) -> None:
        text = format_result({"task_id": "t1", "status": "PASS"})
        self.assertTrue(text.startswith("[PASEO_RESULT v1]"))
        self.assertIn("status: PASS", text)

    def test_completed_json_is_pass_even_if_exit_1(self) -> None:
        text = 'Tip: ignore\n{"agentId":"4d59d1a3-2e6d-42e6-9aed-2a99fdf8c7d4","status":"completed"}\n'
        self.assertEqual(last_json_object(text)["status"], "completed")
        status, err = interpret_outcome(1, text, "- **status:** PASS")
        self.assertEqual(status, "PASS")
        self.assertEqual(err, "")

    def test_comment_is_not_executed(self) -> None:
        # Parser must keep shell-looking text as data only.
        body = f"""{TASK_MARKER}

task_id: safe-001
repo: einklover/m4-stack
goal:
rm -rf /; $(reboot)
"""
        task = parse_task(body)
        self.assertIn("rm -rf /", task.fields["goal"])
        self.assertEqual(task.task_id, "safe-001")


if __name__ == "__main__":
    unittest.main()
