#!/usr/bin/env python3
"""Run the local Murphy M4 validation stack and emit a machine-readable report."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def run_step(name: str, command: list[str], cwd: Path = ROOT) -> dict:
    started = time.monotonic()
    proc = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return {
        "name": name,
        "command": command,
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "duration_ms": round((time.monotonic() - started) * 1000),
        "output": proc.stdout,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", default="build-acceptance")
    p.add_argument("--sanitize", action="store_true", help="also run ASan/UBSan low-level gates")
    p.add_argument("--seeds", default=None, help="optional schedule-fuzz range, e.g. 1:1000")
    p.add_argument("--json", dest="json_path", help="write full report here")
    p.add_argument("--keep-going", action="store_true", help="continue after a failed layer")
    args = p.parse_args(argv)

    build = ROOT / args.build_dir
    steps: list[tuple[str, list[str], Path]] = [
        ("board_contract", [sys.executable, "tools/validate_board_spec.py"], ROOT),
        ("issue_matrix", [sys.executable, "tools/validate_issue_matrix.py"], ROOT),
        ("plugin_matrix", [sys.executable, "tools/validate_plugin_matrix.py"], ROOT),
        ("profile_json", [sys.executable, "-m", "json.tool", "profiles/murphy_m4.json"], ROOT),
        ("configure", ["cmake", "-S", ".", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"], ROOT),
        ("build", ["cmake", "--build", str(build), "-j2"], ROOT),
        ("deterministic", [str(build / "m4_simulator")], ROOT),
        ("board_model", [str(build / "m4_board_tests")], ROOT),
        ("peripheral_policies", [str(build / "m4_peripheral_policies")], ROOT),
        ("issue_contracts", [str(build / "m4_issue_contracts")], ROOT),
        ("native_smoke", [str(build / "m4_native_smoke")], ROOT),
        ("ctest", ["ctest", "--test-dir", str(build), "--output-on-failure"], ROOT),
        ("python_tests", [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"], ROOT),
    ]
    if args.seeds:
        steps.append(("schedule_fuzz", [str(build / "m4_simulator"), "--seeds", args.seeds], ROOT))

    if args.sanitize:
        san = ROOT / (args.build_dir + "-sanitize")
        steps += [
            ("sanitize_configure", ["cmake", "-S", ".", "-B", str(san),
                                    "-DM4SIM_SANITIZE=ON", "-DM4SIM_BUILD_NATIVE_SMOKE=OFF",
                                    "-DCMAKE_BUILD_TYPE=Debug"], ROOT),
            ("sanitize_build", ["cmake", "--build", str(san), "-j2", "--target",
                                "m4_board_tests", "m4_peripheral_policies", "m4_issue_contracts"], ROOT),
            ("sanitize_board", [str(san / "m4_board_tests")], ROOT),
            ("sanitize_peripherals", [str(san / "m4_peripheral_policies")], ROOT),
            ("sanitize_issues", [str(san / "m4_issue_contracts")], ROOT),
        ]

    report = {
        "schema_version": 1,
        "root": str(ROOT),
        "environment": {"python": sys.version.split()[0], "cwd": os.getcwd()},
        "steps": [],
        "ok": True,
    }

    for name, command, cwd in steps:
        print(f"\n=== {name} ===")
        result = run_step(name, command, cwd)
        report["steps"].append(result)
        print(result["output"], end="" if result["output"].endswith("\n") else "\n")
        print(f"[{name}] {'PASS' if result['ok'] else 'FAIL'} {result['duration_ms']}ms")
        if not result["ok"]:
            report["ok"] = False
            if not args.keep_going:
                break

    if args.json_path:
        Path(args.json_path).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    passed = sum(1 for step in report["steps"] if step["ok"])
    print(f"\nAcceptance: {'PASS' if report['ok'] else 'FAIL'} ({passed}/{len(report['steps'])} steps passed)")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
