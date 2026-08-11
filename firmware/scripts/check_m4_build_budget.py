#!/usr/bin/env python3
"""Offline Murphy M4 firmware size gate.  This script never connects to USB."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


APP1_BYTES = 0x6D0000
STATIC_RAM_BYTES = 327_680
DEFAULT_FLASH_FRACTION = 0.85
DEFAULT_RAM_FRACTION = 0.35

SIZE_RE = re.compile(
    r"(?P<kind>RAM|Flash):.*?used\s+(?P<used>[0-9,]+)\s+bytes\s+from\s+"
    r"(?P<total>[0-9,]+)\s+bytes",
    re.IGNORECASE,
)


def parse_size_output(text: str) -> dict[str, tuple[int, int]]:
    result: dict[str, tuple[int, int]] = {}
    for match in SIZE_RE.finditer(text):
        kind = match.group("kind").lower()
        used = int(match.group("used").replace(",", ""))
        total = int(match.group("total").replace(",", ""))
        result[kind] = (used, total)
    return result


def parse_app1_partition(csv_text: str) -> tuple[int, int]:
    for raw in csv_text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cols = [part.strip() for part in line.split(",")]
        if len(cols) >= 5 and cols[0] == "app1":
            return int(cols[3], 0), int(cols[4], 0)
    raise ValueError("app1 partition missing")


def evaluate(
    firmware_size: int,
    measurements: dict[str, tuple[int, int]],
    app1_size: int = APP1_BYTES,
    flash_fraction: float = DEFAULT_FLASH_FRACTION,
    ram_fraction: float = DEFAULT_RAM_FRACTION,
) -> list[str]:
    failures: list[str] = []
    if firmware_size > app1_size:
        failures.append(f"firmware exceeds APP1: {firmware_size} > {app1_size}")
    if firmware_size > int(app1_size * flash_fraction):
        failures.append(
            f"firmware exceeds {flash_fraction:.0%} release budget: "
            f"{firmware_size} > {int(app1_size * flash_fraction)}"
        )
    ram = measurements.get("ram")
    if ram is None:
        failures.append("PlatformIO RAM measurement missing")
    else:
        used, total = ram
        if total != STATIC_RAM_BYTES:
            failures.append(f"unexpected static RAM total: {total} != {STATIC_RAM_BYTES}")
        if used > int(total * ram_fraction):
            failures.append(
                f"static RAM exceeds {ram_fraction:.0%} release budget: "
                f"{used} > {int(total * ram_fraction)}"
            )
    flash = measurements.get("flash")
    if flash is None:
        failures.append("PlatformIO flash measurement missing")
    else:
        used, total = flash
        if total != app1_size:
            failures.append(f"PlatformIO flash total differs from APP1: {total} != {app1_size}")
        if used > int(total * flash_fraction):
            failures.append(
                f"linked flash exceeds {flash_fraction:.0%} release budget: "
                f"{used} > {int(total * flash_fraction)}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--build-log", type=Path, required=True)
    parser.add_argument(
        "--partitions", type=Path, default=Path("partitions_murphy_m4.csv")
    )
    parser.add_argument("--flash-fraction", type=float, default=DEFAULT_FLASH_FRACTION)
    parser.add_argument("--ram-fraction", type=float, default=DEFAULT_RAM_FRACTION)
    args = parser.parse_args()

    if not args.firmware.is_file():
        parser.error(f"firmware not found: {args.firmware}")
    if not args.build_log.is_file():
        parser.error(f"build log not found: {args.build_log}")
    if not args.partitions.is_file():
        parser.error(f"partition table not found: {args.partitions}")
    if not 0 < args.flash_fraction <= 1 or not 0 < args.ram_fraction <= 1:
        parser.error("budget fractions must be in (0, 1]")

    app1_offset, app1_size = parse_app1_partition(
        args.partitions.read_text(encoding="utf-8")
    )
    measurements = parse_size_output(args.build_log.read_text(encoding="utf-8"))
    firmware_size = args.firmware.stat().st_size
    failures = evaluate(
        firmware_size,
        measurements,
        app1_size=app1_size,
        flash_fraction=args.flash_fraction,
        ram_fraction=args.ram_fraction,
    )
    report = {
        "app1_offset": app1_offset,
        "app1_size": app1_size,
        "firmware_size": firmware_size,
        "firmware_headroom": app1_size - firmware_size,
        "measurements": measurements,
        "limits": {
            "flash_fraction": args.flash_fraction,
            "ram_fraction": args.ram_fraction,
        },
        "failures": failures,
    }
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
