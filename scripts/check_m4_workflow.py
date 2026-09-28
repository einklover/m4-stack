#!/usr/bin/env python3
"""Check M4 PR evidence and changed paths. Evidence format is not review proof."""
import argparse
import json
import os
import re
import subprocess
from pathlib import Path

def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()

def classify(paths):
    code = any(p.startswith(("firmware/", "plugins/", "simulator/", "scripts/")) or p == "m4sim" for p in paths)
    build = any(p.startswith(("firmware/src/", "firmware/lib/", "firmware/open-m4-sdk/", "firmware/scripts/"))
                or p in ("firmware/platformio.ini", "scripts/bootstrap_deps.sh") for p in paths)
    high = any(p == "firmware/src/main.cpp" or p.startswith((
        "firmware/src/sd/", "firmware/lib/hal/", "firmware/src/apps/M4xInstall",
        "firmware/src/apps/M4xRegistry")) or "SdFat" in p for p in paths)
    return code, build, high

def forbidden(paths):
    bad = []
    for path in paths:
        parts = Path(path).parts
        name = Path(path).name.lower()
        if (".pio" in parts or name in (".env", ".ds_store", "id_rsa", "credentials.json")
            or name.startswith(".env.") or path.lower().endswith(
                (".m4x", ".elf", ".bin", ".pem", ".key"))):
            bad.append(path)
    return bad

def evidence_fields(body):
    expr = r"(?m)^\s*-\s*\*\*(Risk|Frozen SHA|Targeted tests|Muse review|Astra)\*\*:\s*(.*)$"
    return {m.group(1).lower(): m.group(2).strip() for m in re.finditer(expr, body or "")}

def validate_evidence(body, head, high):
    d = evidence_fields(body)
    errors = []
    for key in ("risk", "frozen sha", "targeted tests", "muse review", "astra"):
        val = d.get(key, "")
        if not val or val.startswith("[") or val.lower().startswith(("todo", "pending", "tbd")):
            errors.append(f"Missing or unfilled PR evidence: {key}")
    if d.get("risk", "").lower() not in ("low", "high"):
        errors.append("Risk must be exactly low or high")
    if high and d.get("risk", "").lower() != "high":
        errors.append("Critical firmware paths require Risk: high")
    if d.get("frozen sha", "").lower() != head.lower():
        errors.append("Frozen SHA must equal the actual PR head SHA; re-review after edits")
    if not d.get("targeted tests", "").lower().startswith("passed:"):
        errors.append("Targeted tests must start with passed: and cite real command/results")
    if not d.get("muse review", "").lower().startswith(("passed:", "findings resolved:")):
        errors.append("Muse review must cite independent review: passed: or findings resolved:")
    astra = d.get("astra", "").lower()
    if astra != "not requested" and not astra.startswith("approved:"):
        errors.append("Astra must be not requested or approved: <user authorization reference>")
    return errors

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--base-ref", required=True)
    p.add_argument("--pr-body-file")
    args = p.parse_args()
    head = git("rev-parse", "HEAD")
    base = git("merge-base", args.base_ref, head)
    paths = git("diff", "--name-only", "--diff-filter=ACMRD", base, head).splitlines()
    code, build, high = classify(paths)
    errors = [f"Forbidden committed artifact: {path}" for path in forbidden(paths)]
    if code and os.getenv("GITHUB_EVENT_NAME") == "pull_request":
        if args.pr_body_file:
            body = Path(args.pr_body_file).read_text()
        else:
            body = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())["pull_request"].get("body") or ""
        errors.extend(validate_evidence(body, head, high))
    if os.getenv("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            out.write(f"firmware_build={'true' if build else 'false'}\n")
            out.write(f"code_changed={'true' if code else 'false'}\n")
    print(f"M4 gate base={base[:12]} head={head[:12]} changed={len(paths)} "
          f"code={code} build={build} high_risk={high}")
    for err in errors:
        print("ERROR:", err)
    raise SystemExit(bool(errors))

if __name__ == "__main__":
    main()
