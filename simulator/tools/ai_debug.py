#!/usr/bin/env python3
"""One-command, graded Murphy M4 simulator interface for coding agents."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
LEVELS = [
    {
        "id": 0,
        "name": "contracts",
        "cost": "~1s",
        "proves": "board/issue schemas and Python tooling contracts",
        "does_not_prove": "compiled C++ or firmware behavior",
    },
    {
        "id": 1,
        "name": "model",
        "cost": "~5s warm",
        "proves": "deterministic reader, heap, storage, GPIO and SSD1677 contracts",
        "does_not_prove": "Xtensa/FreeRTOS execution",
    },
    {
        "id": 2,
        "name": "fuzz",
        "cost": "seed dependent",
        "proves": "scheduler invariants across deterministic seed permutations",
        "does_not_prove": "real ESP32-S3 runtime behavior",
    },
    {
        "id": 3,
        "name": "qemu-boot",
        "cost": "~1min cold",
        "proves": "16MiB ROM/bootloader/Xtensa/FreeRTOS/app setup execution",
        "does_not_prove": "physical SSD1677, SD, touch or RF behavior",
    },
    {
        "id": 4,
        "name": "qemu-screen",
        "cost": "~1min cold",
        "proves": "real firmware UI composition and a stable 480x800 PBM frame",
        "does_not_prove": "EPD waveform appearance, interactive input or real hardware",
    },
]


def _tool(name: str, fallback: Path | None = None) -> str:
    found = shutil.which(name)
    if found:
        return found
    if fallback and fallback.is_file():
        return str(fallback)
    return name


def selected_levels(level: int, through: bool) -> list[int]:
    return list(range(level + 1)) if through else [level]


def build_steps(args: argparse.Namespace) -> tuple[list[dict], dict[str, Path]]:
    build = ROOT / "build"
    output = Path(args.output_dir).expanduser().resolve()
    firmware = Path(args.firmware_dir).expanduser().resolve()
    flash = output / "murphy-qemu.bin"
    serial = output / "qemu-serial.log"
    screen = output / "qemu-screen.pbm"
    probe = serial.with_suffix(serial.suffix + ".probe.json")
    py = sys.executable
    cmake = _tool("cmake")
    ctest = _tool("ctest")
    pio = _tool("pio", Path.home() / ".platformio/penv/bin/pio")
    levels = selected_levels(args.level, args.through)
    steps: list[dict] = []

    def add(name: str, cmd: list[str], cwd: Path = ROOT) -> None:
        if not any(step["name"] == name for step in steps):
            steps.append({"name": name, "command": cmd, "cwd": cwd})

    if 0 in levels:
        add("board-spec", [py, "tools/validate_board_spec.py"])
        add("issue-matrix", [py, "tools/validate_issue_matrix.py"])
        add("python-tests", [py, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"])
    if 1 in levels or 2 in levels:
        add("cmake-configure", [cmake, "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"])
        add("cmake-build", [cmake, "--build", "build", "-j"])
    if 1 in levels:
        add("ctest", [ctest, "--test-dir", "build", "--output-on-failure"])
        add("scenario-suite", [str(build / "m4_simulator")])
    if 2 in levels:
        fuzz = [str(build / "m4_simulator"), "--seeds", args.seeds]
        if args.scenario:
            fuzz.append(args.scenario)
        add("scenario-fuzz", fuzz)
    if 3 in levels or 4 in levels:
        add("firmware-build", [pio, "run", "-e", "murphy_m4_qemu"], firmware)
        add(
            "flash-compose",
            [
                py,
                "tools/murphy_flash_image.py",
                "--build-dir",
                str(firmware / ".pio/build/murphy_m4_qemu"),
                "-o",
                str(flash),
            ],
        )
    if 3 in levels:
        add(
            "qemu-boot",
            [
                py,
                "qemu/run_murphy_bin.py",
                str(flash),
                "--seconds",
                str(args.qemu_seconds),
                "--serial-file",
                str(serial),
                "--probe",
            ],
        )
    if 4 in levels:
        add(
            "qemu-screen",
            [
                py,
                "qemu/run_murphy_bin.py",
                str(flash),
                "--seconds",
                str(args.qemu_seconds),
                "--serial-file",
                str(serial),
                "--screen-file",
                str(screen),
                "--probe",
            ],
        )

    return steps, {
        "output_dir": output,
        "summary": output / "summary.json",
        "flash": flash,
        "serial": serial,
        "probe": probe,
        "screen": screen,
    }


def _tail(path: Path, lines: int = 30) -> list[str]:
    try:
        raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
        useful = [
            line[:500]
            for line in raw
            if not line.startswith("[M4-QEMU-FB] D ")
            and not (len(line) > 128 and set(line) <= set("0123456789ABCDEF"))
        ]
        return useful[-lines:]
    except OSError:
        return []


def _save(summary: dict, path: Path) -> None:
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _validate_qemu(summary: dict, artifacts: dict[str, Path], need_screen: bool) -> str | None:
    try:
        probe = json.loads(artifacts["probe"].read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return f"missing or invalid QEMU probe: {exc}"
    acceptance = probe.get("acceptance", {})
    if not acceptance.get("firmware_setup_started"):
        return "QEMU did not reach firmware setup()"
    if probe.get("failure_class") is not None:
        return f"QEMU failure_class={probe['failure_class']}"
    summary["qemu_probe"] = probe
    if need_screen:
        try:
            screen = artifacts["screen"].read_bytes()
        except OSError as exc:
            return f"missing screen artifact: {exc}"
        header = screen[:11]
        if header != b"P4\n480 800\n":
            return f"unexpected screen header: {header!r}"
        pixels = screen[11:]
        if len(pixels) != 48000 or not any(pixels):
            return f"blank or truncated screen: {len(pixels)} payload bytes"
        summary["screen"] = {
            "format": "PBM P4",
            "width": 480,
            "height": 800,
            "black_pixels": sum(byte.bit_count() for byte in pixels),
        }
    return None


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("level", nargs="?", type=int, default=0, choices=range(5))
    parser.add_argument("--through", action="store_true", help="run every level from 0 through LEVEL")
    parser.add_argument("--list", action="store_true", help="describe levels and exit")
    parser.add_argument("--json", action="store_true", help="print only the final JSON summary")
    parser.add_argument("--dry-run", action="store_true", help="write the plan without executing commands")
    parser.add_argument("--scenario", help="level 2: fuzz only this deterministic scenario")
    parser.add_argument("--seeds", default="1:200", help="level 2 seed range (default: 1:200)")
    parser.add_argument("--qemu-seconds", type=float, default=12.0)
    parser.add_argument("--timeout", type=float, default=600.0, help="per-step timeout")
    parser.add_argument(
        "--firmware-dir",
        default=str(ROOT.parent / "wap-checkpoint/firmware"),
    )
    parser.add_argument("--output-dir", default=str(ROOT / "build/ai-debug"))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.list:
        rendered = json.dumps({"schema_version": 1, "levels": LEVELS}, ensure_ascii=False, indent=2)
        print(rendered if args.json else rendered)
        return 0

    steps, artifacts = build_steps(args)
    artifacts["output_dir"].mkdir(parents=True, exist_ok=True)
    summary = {
        "schema_version": 1,
        "status": "dry_run" if args.dry_run else "running",
        "requested": {"level": args.level, "through": args.through},
        "levels": [LEVELS[i] for i in selected_levels(args.level, args.through)],
        "steps": [],
        "artifacts": {key: str(value) for key, value in artifacts.items()},
    }
    if args.dry_run:
        summary["steps"] = [
            {"name": step["name"], "command": step["command"], "cwd": str(step["cwd"]), "status": "planned"}
            for step in steps
        ]
        _save(summary, artifacts["summary"])
        print(json.dumps(summary, ensure_ascii=False, indent=2) if args.json else f"plan: {artifacts['summary']}")
        return 0

    failed = False
    for step in steps:
        log = artifacts["output_dir"] / f"{step['name']}.log"
        started = time.monotonic()
        if not args.json:
            print(f"[{step['name']}] running", flush=True)
        try:
            with log.open("w", encoding="utf-8") as stream:
                stream.write("+ " + " ".join(step["command"]) + "\n")
                stream.flush()
                result = subprocess.run(
                    step["command"],
                    cwd=step["cwd"],
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    timeout=args.timeout,
                    env=os.environ.copy(),
                    check=False,
                )
            returncode = result.returncode
        except subprocess.TimeoutExpired:
            returncode = 124
        except OSError as exc:
            log.write_text(f"launch error: {exc}\n", encoding="utf-8")
            returncode = 127
        record = {
            "name": step["name"],
            "status": "pass" if returncode == 0 else "fail",
            "returncode": returncode,
            "duration_s": round(time.monotonic() - started, 3),
            "command": step["command"],
            "cwd": str(step["cwd"]),
            "log": str(log),
            "tail": _tail(log),
        }
        summary["steps"].append(record)
        if not args.json:
            print(f"[{step['name']}] {record['status']} ({record['duration_s']}s)", flush=True)
        if returncode != 0:
            failed = True
            summary["failed_step"] = step["name"]
            break
        _save(summary, artifacts["summary"])

    ran_names = {step["name"] for step in summary["steps"]}
    qemu_error = None
    if not failed and "qemu-screen" in ran_names:
        qemu_error = _validate_qemu(summary, artifacts, need_screen=True)
    elif not failed and "qemu-boot" in ran_names:
        qemu_error = _validate_qemu(summary, artifacts, need_screen=False)
    if qemu_error:
        failed = True
        summary["failed_step"] = "qemu-acceptance"
        summary["error"] = qemu_error

    summary["status"] = "fail" if failed else "pass"
    if failed:
        summary["next"] = f"inspect {summary['steps'][-1]['log']} and its tail"
    elif args.level < 4:
        summary["next"] = f"run level {args.level + 1} only if the issue needs higher fidelity"
    else:
        summary["next"] = "physical M4 is still required for EPD appearance, RF and electrical claims"
    _save(summary, artifacts["summary"])
    rendered = json.dumps(summary, ensure_ascii=False, indent=2)
    print(rendered if args.json else f"{summary['status']}: {artifacts['summary']}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
