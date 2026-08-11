#!/usr/bin/env python3
"""Validate cross-repository plugin regression ownership and local mappings."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

VALID_STATUS = {"cross_repo_covered", "upstream_application_regression"}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def validate(matrix: dict, root: Path) -> list[str]:
    errors: list[str] = []
    seen: set[str] = set()
    scenario_text = (root / "scenarios" / "scenarios.cpp").read_text(encoding="utf-8")
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    scenarios = set(re.findall(r'"([a-z0-9_]+)"', scenario_text))
    ctests = set(re.findall(r"add_test\(NAME\s+([A-Za-z0-9_.-]+)", cmake_text))

    for entry in matrix.get("entries", []):
        eid = entry.get("id", "")
        if not eid:
            errors.append("entry missing id")
            continue
        if eid in seen:
            errors.append(f"duplicate plugin regression id: {eid}")
        seen.add(eid)
        if entry.get("status") not in VALID_STATUS:
            errors.append(f"{eid}: invalid status {entry.get('status')!r}")
        repo = entry.get("source_repo", "")
        if not repo.startswith("einklover/"):
            errors.append(f"{eid}: source_repo must be an explicit einklover repository")
        sha = entry.get("source_commit", "")
        if not SHA_RE.fullmatch(sha):
            errors.append(f"{eid}: source_commit must be a full 40-hex SHA")
        if not entry.get("symptom"):
            errors.append(f"{eid}: symptom required")
        if not entry.get("upstream_tests"):
            errors.append(f"{eid}: upstream_tests required")

        for ref in entry.get("local_contracts", []):
            if ref.startswith("scenario:"):
                name = ref.split(":", 1)[1]
                if name not in scenarios:
                    errors.append(f"{eid}: unknown local scenario {name}")
            elif ref.startswith("ctest:"):
                name = ref.split(":", 1)[1]
                if name not in ctests:
                    errors.append(f"{eid}: unknown local CTest {name}")
            else:
                errors.append(f"{eid}: invalid local contract ref {ref!r}")

        if entry.get("status") == "cross_repo_covered" and not entry.get("local_contracts"):
            errors.append(f"{eid}: cross_repo_covered requires at least one local cross-cutting contract")

    return errors


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("matrix", nargs="?", default="issues/plugin_regressions.json")
    args = p.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    try:
        matrix = json.loads((root / args.matrix).read_text(encoding="utf-8"))
        errors = validate(matrix, root)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"plugin matrix error: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print(f"PASS plugin matrix: {args.matrix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
