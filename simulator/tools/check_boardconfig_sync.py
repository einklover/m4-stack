#!/usr/bin/env python3
"""Cross-check simulator Murphy pins against current FreeInk BoardConfig.

This is intentionally a narrow parser for the `constexpr BoardProfile MURPHY_M4`
initializer, not a general C++ parser. If upstream substantially restructures the
profile, this gate fails closed and asks us to review the new hardware truth.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


def murphy_block(text: str) -> str:
    marker = "constexpr BoardProfile MURPHY_M4 = {"
    start = text.find(marker)
    if start < 0:
        raise ValueError("MURPHY_M4 BoardProfile initializer not found")
    end_marker = "// --- de-link"
    end = text.find(end_marker, start)
    if end < 0:
        raise ValueError("MURPHY_M4 initializer end sentinel not found")
    return text[start:end]


def parse(block: str) -> dict:
    clean = re.sub(r"//[^\n]*", " ", block)
    clean = re.sub(r"/\*.*?\*/", " ", clean, flags=re.S)

    def req(pattern: str, label: str):
        m = re.search(pattern, clean, re.S)
        if not m:
            raise ValueError(f"could not extract {label} from MURPHY_M4 initializer")
        return m

    head = req(
        r'Board::MurphyM4\s*,\s*"murphy_m4"\s*,\s*InputStyle::DigitalButtons\s*,\s*'
        r'DisplayController::SSD1677\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*'
        r'\{\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*PIN_UNASSIGNED\s*\}\s*,\s*(\d+)',
        "display",
    )
    sdcompat = req(
        r'\{\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*(\d+)\s*,\s*(true|false)\s*,\s*0\s*,\s*(true|false)\s*\}',
        "SD compatibility/power",
    )
    keys = req(
        r'\{\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*PIN_UNASSIGNED\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(true|false)\s*\}',
        "input pins",
    )
    battery = req(r'\}\s*,\s*(\d+)\s*,\s*PIN_UNASSIGNED\s*,\s*([0-9.]+)f', "battery")
    touch = req(
        r'\{\s*TouchController::Ft6x36\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*PIN_UNASSIGNED\s*,\s*0x([0-9a-fA-F]+)\s*,\s*'
        r'(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(true|false)\s*,\s*0\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*(\d+)\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*(true|false)\s*\}',
        "touch",
    )
    light = req(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(true|false)\s*,\s*(\d+)\s*\}', "frontlight")
    sdmmc = req(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*,\s*NO_GAUGE', "SDMMC")

    as_bool = lambda v: v == "true"
    return {
        "display": {
            "width": int(head.group(1)), "height": int(head.group(2)),
            "sclk": int(head.group(3)), "mosi": int(head.group(4)), "cs": int(head.group(5)),
            "dc": int(head.group(6)), "reset": int(head.group(7)), "busy": int(head.group(8)),
            "spi_hz": int(head.group(9)),
        },
        # SdPins fields are powerEnable, separateSpi, spiHz, powerActiveHigh.
        "sd_power": {"pin": int(sdcompat.group(1)), "active_high": as_bool(sdcompat.group(3))},
        "input": {"up": int(keys.group(1)), "down": int(keys.group(2)), "power": int(keys.group(3)),
                  "active_high": as_bool(keys.group(4))},
        "battery": {"adc": int(battery.group(1)), "multiplier": float(battery.group(2))},
        "touch": {
            "sda": int(touch.group(1)), "scl": int(touch.group(2)), "irq": int(touch.group(3)),
            "address": int(touch.group(4), 16), "raw_min_x": int(touch.group(5)),
            "raw_max_x": int(touch.group(6)), "raw_min_y": int(touch.group(7)),
            "raw_max_y": int(touch.group(8)), "power": int(touch.group(12)),
            "swap_xy": as_bool(touch.group(13)), "flip_x": as_bool(touch.group(14)),
            "flip_y": as_bool(touch.group(15)), "power_active_high": as_bool(touch.group(17)),
        },
        "frontlight": {"cool": int(light.group(1)), "hz": int(light.group(2)),
                       "bits": int(light.group(3)), "active_high": as_bool(light.group(4)),
                       "warm": int(light.group(5))},
        "sdmmc": {"clk": int(sdmmc.group(1)), "cmd": int(sdmmc.group(2)),
                   "d0": int(sdmmc.group(3)), "d1": int(sdmmc.group(4)),
                   "d2": int(sdmmc.group(5)), "d3": int(sdmmc.group(6)),
                   "width": int(sdmmc.group(7))},
        "sensors_disabled": bool(re.search(r'NO_SENSORS\s*,\s*1\.2f\s*\};', clean)),
    }


def expected_from_spec(spec: dict) -> dict:
    return {
        "display": {
            "width": spec["display"]["width"], "height": spec["display"]["height"],
            "sclk": spec["display"]["pins"]["sclk"], "mosi": spec["display"]["pins"]["mosi"],
            "cs": spec["display"]["pins"]["cs"], "dc": spec["display"]["pins"]["dc"],
            "reset": spec["display"]["pins"]["reset"], "busy": spec["display"]["pins"]["busy"],
            "spi_hz": spec["display"]["spi_hz"],
        },
        "sd_power": {"pin": spec["sd"]["pins"]["power"], "active_high": spec["sd"]["power_active_high"]},
        "input": {"up": spec["input"]["pins"]["up"], "down": spec["input"]["pins"]["down"],
                  "power": spec["input"]["pins"]["lock_power"], "active_high": not spec["input"]["active_low"]},
        "battery": {"adc": spec["battery"]["adc_pin"], "multiplier": spec["battery"]["divider_multiplier"]},
        "touch": {
            "sda": spec["touch"]["pins"]["sda"], "scl": spec["touch"]["pins"]["scl"],
            "irq": spec["touch"]["pins"]["irq"], "address": spec["touch"]["i2c_address"],
            "raw_min_x": 0, "raw_max_x": 799, "raw_min_y": 0, "raw_max_y": 479,
            "power": spec["touch"]["pins"]["power"], "swap_xy": spec["touch"]["transform"]["swap_xy"],
            "flip_x": spec["touch"]["transform"]["flip_x"], "flip_y": spec["touch"]["transform"]["flip_y"],
            "power_active_high": spec["touch"]["power_active_high"],
        },
        "frontlight": {"cool": spec["frontlight"]["pins"]["cool"], "hz": spec["frontlight"]["pwm_hz"],
                       "bits": spec["frontlight"]["resolution_bits"], "active_high": spec["frontlight"]["active_high"],
                       "warm": spec["frontlight"]["pins"]["warm"]},
        "sdmmc": {"clk": spec["sd"]["pins"]["clk"], "cmd": spec["sd"]["pins"]["cmd"],
                   "d0": spec["sd"]["pins"]["d0"], "d1": spec["sd"]["pins"]["d1"],
                   "d2": spec["sd"]["pins"]["d2"], "d3": spec["sd"]["pins"]["d3"],
                   "width": spec["sd"]["bus_width"]},
        "sensors_disabled": not any(d.get("firmware_enabled") for d in spec["physical_i2c_devices"] if d["name"] != "touch"),
    }


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("boardconfig")
    p.add_argument("--spec", default="board/murphy_m4.json")
    args = p.parse_args(argv)
    try:
        actual = parse(murphy_block(Path(args.boardconfig).read_text(encoding="utf-8")))
        expected = expected_from_spec(json.loads(Path(args.spec).read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"boardconfig sync error: {exc}", file=sys.stderr)
        return 2
    if actual != expected:
        print("ERROR: simulator board profile drifted from FreeInk MURPHY_M4", file=sys.stderr)
        print("actual BoardConfig:\n" + json.dumps(actual, indent=2), file=sys.stderr)
        print("expected simulator:\n" + json.dumps(expected, indent=2), file=sys.stderr)
        return 1
    print("PASS FreeInk MURPHY_M4 and simulator board contract match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
