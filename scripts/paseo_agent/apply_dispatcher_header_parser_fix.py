#!/usr/bin/env python3
"""Apply the ChatGPT-authored Paseo task-header parser hardening.

This script intentionally performs exact source replacements so a local execution
agent can apply the reviewed change mechanically without redesigning the parser.
It patches both the runner and its regression tests.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts/paseo_agent/paseo_agent_runner.py"
TESTS = ROOT / "scripts/paseo_agent/test_paseo_agent_runner.py"


def replace_exact(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match in {path}, got {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"OK {label}: {path}")


def main() -> None:
    replace_exact(
        RUNNER,
        '''    "timeout",\n    "provider",\n    "goal",\n''',
        '''    "timeout",\n    "provider",\n    "workspace_id",\n    "goal",\n''',
        "register workspace_id header",
    )

    replace_exact(
        RUNNER,
        '''INSPECT_ONLY_MODES = {"inspect-only", "inspect", "read-only"}\nRESULT_FILE_NAME = ".paseo-agent-result.md"\n''',
        '''# These fields are single-line control-plane scalars. If an unknown\n# ``name: value`` header follows one of them, it must terminate the scalar\n# instead of being appended as a continuation line. Free-form fields such as\n# goal/context/acceptance intentionally retain colon-bearing prose.\nSCALAR_TASK_KEYS = frozenset({\n    "task_id",\n    "issue",\n    "repo",\n    "branch",\n    "mode",\n    "expected_head",\n    "timeout",\n    "provider",\n    "workspace_id",\n})\n\nINSPECT_ONLY_MODES = {"inspect-only", "inspect", "read-only"}\nRESULT_FILE_NAME = ".paseo-agent-result.md"\n''',
        "define scalar task headers",
    )

    replace_exact(
        RUNNER,
        '''        if key_match:\n            key = key_match.group(1).lower()\n            if key in KNOWN_KEYS:\n                current = key\n                values.setdefault(current, [])\n                rest = key_match.group(2)\n                if rest != "":\n                    values[current].append(rest)\n                continue\n        if current is None:\n            continue\n        values.setdefault(current, []).append(line)\n''',
        '''        if key_match:\n            key = key_match.group(1).lower()\n            if key in KNOWN_KEYS:\n                current = key\n                values.setdefault(current, [])\n                rest = key_match.group(2)\n                if rest != "":\n                    values[current].append(rest)\n                continue\n            # Unknown header-looking lines must never extend a preceding scalar\n            # such as timeout/provider. This is what previously turned\n            # ``timeout: 120m`` + ``workspace_id: ...`` into an invalid timeout.\n            # Preserve colon-bearing prose when already inside a free-form field.\n            if current in SCALAR_TASK_KEYS:\n                current = None\n                continue\n        if current is None:\n            continue\n        values.setdefault(current, []).append(line)\n''',
        "harden unknown-header continuation",
    )

    insert_after = '''    def test_timeout_parse(self) -> None:\n        self.assertEqual(parse_timeout("90m"), 5400)\n        self.assertEqual(parse_timeout("2h"), 7200)\n        self.assertEqual(parse_timeout("180m"), 10800)\n\n'''
    new_tests = insert_after + '''    def test_workspace_id_header_does_not_poison_timeout_or_provider(self) -> None:\n        body = f"""{TASK_MARKER}\n\ntask_id: parser-workspace-001\ntimeout: 120m\nprovider: grok\nworkspace_id: wks_c7bfd8e08671b105\ngoal:\nValidate device flow.\n"""\n        task = parse_task(body)\n        self.assertEqual(task.timeout_sec, 7200)\n        self.assertEqual(task.provider, "grok")\n        self.assertEqual(task.fields["workspace_id"], "wks_c7bfd8e08671b105")\n\n    def test_unknown_header_cannot_extend_scalar(self) -> None:\n        body = f"""{TASK_MARKER}\n\ntask_id: parser-future-001\ntimeout: 120m\nfuture_header: future-value\ngoal:\nContinue normally.\n"""\n        task = parse_task(body)\n        self.assertEqual(task.timeout_sec, 7200)\n        self.assertEqual(task.provider, "grok")\n        self.assertIn("Continue normally.", task.fields["goal"])\n\n    def test_colon_prose_is_preserved_in_freeform_field(self) -> None:\n        body = f"""{TASK_MARKER}\n\ntask_id: parser-prose-001\ngoal:\nNote: keep this line as goal prose.\nAnother line.\nacceptance:\n- parsed\n"""\n        task = parse_task(body)\n        self.assertIn("Note: keep this line as goal prose.", task.fields["goal"])\n        self.assertIn("Another line.", task.fields["goal"])\n\n'''
    replace_exact(TESTS, insert_after, new_tests, "add parser regression tests")

    print("PASS: dispatcher parser hardening applied (4 exact replacements)")


if __name__ == "__main__":
    main()
