#!/usr/bin/env python3
"""Require every real m4-firmware issue to be classified in regressions.json.

GitHub's issues REST endpoint also returns PRs; those are explicitly filtered.
This is a network gate for CI, not part of the offline fast acceptance command.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request


def fetch(repo: str, token: str | None = None) -> list[dict]:
    url = f"https://api.github.com/repos/{repo}/issues?state=all&per_page=100"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=20) as response:
        return json.load(response)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", default="einklover/m4-firmware")
    p.add_argument("--matrix", default="issues/regressions.json")
    args = p.parse_args(argv)
    try:
        matrix = json.load(open(args.matrix, encoding="utf-8"))
        remote = fetch(args.repo, os.environ.get("GITHUB_TOKEN"))
    except (OSError, ValueError) as exc:
        print(f"live issue coverage error: {exc}", file=sys.stderr)
        return 2

    real_issues = {int(item["number"]) for item in remote if "pull_request" not in item}
    covered = {int(entry["issue"]) for entry in matrix.get("entries", [])}
    missing = sorted(real_issues - covered)
    stale = sorted(covered - real_issues)
    if missing:
        print(f"ERROR: unclassified m4-firmware issues: {missing}", file=sys.stderr)
        return 1
    if stale:
        print(f"ERROR: matrix references non-issue numbers: {stale}", file=sys.stderr)
        return 1
    print(f"PASS live issue coverage: {len(real_issues)} issues classified ({sorted(real_issues)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
