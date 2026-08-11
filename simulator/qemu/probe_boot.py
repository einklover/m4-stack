#!/usr/bin/env python3
"""Classify how far a real Murphy firmware image gets in ESP32-S3 QEMU."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

STAGES = [
    ("rom", [r"ESP-ROM", r"rst:0x", r"boot:0x"]),
    (
        "bootloader",
        [
            r"second stage bootloader",
            r"Partition Table",
            r"Loading app partition",
            r"entry 0x[0-9a-f]+",
        ],
    ),
    (
        "app",
        [
            r"app_main",
            r"Arduino",
            r"\[M4-RC1\] setup\(\) start",
            r"Starting CrossPoint",
            # Production USB-CDC Serial may be silent on QEMU UART0; Arduino/
            # ESP-IDF host logs still prove the guest app is executing.
            r"esp32-hal-",
            r"sdmmc_common",
            r"esp_psram",
            r"cpu_start",
        ],
    ),
    ("psram", [r"\[M4-PSRAM\]", r"spiram", r"SPIRAM", r"esp_psram"]),
    (
        "board_init",
        [
            r"\[M4-SD\]",
            r"\[M4-DISP\]",
            r"\[M4-TOUCH\]",
            r"BoardConfig",
            r"FreeInk",
            r"sdmmc_init",
            r"i2cWrite",
            r"i2c_master",
        ],
    ),
]

# Exact production boot checkpoints from m4-firmware/src/main.cpp. They are kept
# separate from the coarse STAGES so CI can remain stable while providing a
# much more actionable 'last firmware checkpoint' for the next device model.
FIRMWARE_STAGES = [
    ("setup_start", [r"\[M4-RC1\] setup\(\) start"]),
    ("psram_probe", [r"\[M4-PSRAM\]"]),
    ("frontlight_early", [r"\[M4-LIGHT\] early-boot"]),
    ("sd_begin", [r"\[M4-SD\] first begin", r"\[M4-SD\] mounted ok", r"\[M4-SD\] ERROR"]),
    ("sd_ready", [r"\[M4-SD\] mounted ok"]),
    ("display_ready", [r"\[M4-DISP\] setupDisplayAndFonts done", r"\[M4-DISP\] Display initialized"]),
    ("touch_settle", [r"\[M4-TOUCH\] configured="]),
    ("font_load", [r"\[DBG\] loadFontsFromSd done", r"\[M4-FONT\] LOAD_RESULT"]),
    ("boot_summary", [r"\[M4-RC1\] BOOT_SUMMARY"]),
    ("home_or_reader", [r"\[MAIN\] home1", r"\[MAIN\] reader"]),
]

FAILURES = [
    ("panic", [r"Guru Meditation", r"panic'ed", r"panic reason=", r"LoadProhibited", r"StoreProhibited"]),
    ("watchdog", [r"Task watchdog", r"\bWDT\b", r"watchdog"]),
    ("psram", [r"\[M4-PSRAM\].*(?:not detected|WARNING|fail)", r"spiram.*fail"]),
    ("epd_busy_wait", [r"SSD1677.*BUSY", r"wait.*busy", r"EPD.*busy"]),
    ("sdmmc", [r"\[M4-SD\].*ERROR", r"SDMMC.*(?:fail|error)", r"mount.*fail"]),
    ("i2c_touch", [r"FT6.*(?:fail|NACK)", r"touch.*fail", r"I2C.*NACK", r"i2c.*error"]),
    ("wifi", [r"WiFi.*fail", r"wifi.*error", r"ESP_ERR_WIFI"]),
    ("unsupported_mmio", [r"unimplemented", r"unsupported.*(?:MMIO|register|peripheral)", r"invalid.*MMIO", r"unknown.*register"]),
]


def _matches(patterns: list[str], text: str) -> bool:
    return any(re.search(p, text, re.I) for p in patterns)


def _highest_hits(lines: list[str], stages: list[tuple[str, list[str]]]):
    highest = "none"
    highest_index = -1
    hits = []
    for i, (stage, patterns) in enumerate(stages):
        for n, line in enumerate(lines, 1):
            if _matches(patterns, line):
                hits.append({"stage": stage, "line": n, "raw": line})
                if i > highest_index:
                    highest_index = i
                    highest = stage
                break
    return highest, highest_index, hits


def classify(text: str) -> dict:
    lines = text.splitlines()
    highest, highest_index, stage_hits = _highest_hits(lines, STAGES)
    firmware_highest, firmware_index, firmware_hits = _highest_hits(lines, FIRMWARE_STAGES)

    failure = None
    failure_line = None
    for n, line in enumerate(lines, 1):
        for kind, patterns in FAILURES:
            if _matches(patterns, line):
                failure = kind
                failure_line = {"line": n, "raw": line}
                break
        if failure:
            break

    return {
        "schema_version": 2,
        "highest_stage": highest,
        "stage_hits": stage_hits,
        "highest_firmware_checkpoint": firmware_highest,
        "firmware_checkpoint_hits": firmware_hits,
        "failure_class": failure,
        "failure_line": failure_line,
        "line_count": len(lines),
        "acceptance": {
            "cpu_booted": highest_index >= 0,
            "second_stage_bootloader_reached": highest_index >= 1,
            "application_reached": highest_index >= 2,
            "board_initialization_reached": highest_index >= 4,
            "firmware_setup_started": firmware_index >= 0,
            "boot_summary_reached": firmware_highest in {"boot_summary", "home_or_reader"},
            "home_or_reader_reached": firmware_highest == "home_or_reader",
        },
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", help="captured QEMU monitor/serial log or '-' for stdin")
    p.add_argument("-o", "--output")
    args = p.parse_args(argv)
    try:
        text = sys.stdin.read() if args.log == "-" else Path(args.log).read_text(
            encoding="utf-8", errors="replace"
        )
        result = classify(text)
        rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
    except OSError as exc:
        print(f"boot probe error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
