#!/usr/bin/env python3
"""Validate that issue reproduction claims remain executable and honest."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

VALID_STATUS = {
    "reproduced",
    "modeled",
    "device_trace_required",
    "fixed_upstream_regression",
}


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def local_scenarios(root: Path) -> set[str]:
    text = (root / "scenarios" / "scenarios.cpp").read_text(encoding="utf-8")
    # Registration at allScenarios() contains the public CLI names as string
    # literals; checking the literal is intentionally independent of function names.
    return set(re.findall(r'"([a-z0-9_]+)"', text))


def local_ctests(root: Path) -> set[str]:
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"add_test\(NAME\s+([A-Za-z0-9_.-]+)", text))


def validate(matrix: dict, root: Path) -> list[str]:
    errors: list[str] = []
    seen: set[str] = set()
    scenarios = local_scenarios(root)
    ctests = local_ctests(root)

    for entry in matrix.get("entries", []):
        eid = entry.get("id", "")
        if not eid:
            errors.append("entry missing id")
            continue
        if eid in seen:
            errors.append(f"duplicate entry id: {eid}")
        seen.add(eid)
        status = entry.get("status")
        if status not in VALID_STATUS:
            errors.append(f"{eid}: invalid status {status!r}")
        if not isinstance(entry.get("issue"), int) or entry["issue"] <= 0:
            errors.append(f"{eid}: invalid issue number")
        if not entry.get("reproduction_scope"):
            errors.append(f"{eid}: reproduction_scope is required")
        if not entry.get("evidence"):
            errors.append(f"{eid}: evidence is required")

        refs = entry.get("test_refs", [])
        local_refs = 0
        for ref in refs:
            if ref.startswith("scenario:"):
                name = ref.split(":", 1)[1]
                local_refs += 1
                if name not in scenarios:
                    errors.append(f"{eid}: unknown simulator scenario {name}")
            elif ref.startswith("ctest:"):
                name = ref.split(":", 1)[1]
                local_refs += 1
                if name not in ctests:
                    errors.append(f"{eid}: unknown CTest target {name}")
            elif ref.startswith("firmware:"):
                pass  # upstream regression, connector/firmware CI owns it
            else:
                errors.append(f"{eid}: unsupported test ref {ref!r}")

        if status == "reproduced" and local_refs == 0:
            errors.append(f"{eid}: reproduced claim must reference a local executable test")
        if status == "device_trace_required" and not entry.get("physical_limitations"):
            errors.append(f"{eid}: device-trace-required entry must state physical limitation")

    if not matrix.get("policy") or not (root / matrix["policy"]).is_file():
        errors.append("matrix policy file missing")
    return errors


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("matrix", nargs="?", default="issues/regressions.json")
    args = p.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    try:
        errors = validate(load(root / args.matrix), root)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"issue matrix error: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print(f"PASS issue matrix: {args.matrix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
