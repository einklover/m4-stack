#!/usr/bin/env python3
"""Validate the machine-readable Murphy M4 board contract.

The validator intentionally separates firmware/live-probe truth from schematic-only
facts.  It catches accidental GPIO aliasing, broken shared-I2C declarations, wrong
framebuffer geometry, and attempts to silently erase known source discrepancies.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


class SpecError(RuntimeError):
    pass


def load(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _pin_items(spec: dict):
    sections = ("display", "sd", "input", "touch", "frontlight")
    for section in sections:
        for role, pin in spec[section].get("pins", {}).items():
            if pin is not None:
                yield f"{section}.{role}", int(pin)
    yield "battery.adc", int(spec["battery"]["adc_pin"])
    yield "buzzer.gpio", int(spec["buzzer"]["gpio"])


def validate(spec: dict) -> list[str]:
    errors: list[str] = []
    if spec.get("board") != "murphy_m4":
        errors.append("board must be murphy_m4")

    display = spec["display"]
    expected_fb = int(display["width"]) * int(display["height"]) // 8
    if int(display["framebuffer_bytes"]) != expected_fb:
        errors.append(
            f"framebuffer_bytes={display['framebuffer_bytes']} but geometry requires {expected_fb}"
        )
    if display.get("controller") != "SSD1677":
        errors.append("Murphy M4 executable profile must use SSD1677")
    if display.get("busy_polarity") != "active_high":
        errors.append("SSD1677 BUSY polarity must remain active_high")

    # Primary owners should not alias unless the pin is explicitly declared as
    # a shared bus.  The current live-probed profile is deliberately almost a
    # complete 0..18 allocation, making accidental pin regressions easy to spot.
    shared = {
        int(pin)
        for pins in spec.get("gpio_shared_groups", {}).values()
        for pin in pins
    }
    owners: dict[int, list[str]] = {}
    for role, pin in _pin_items(spec):
        if pin < 0 or pin > 48:
            errors.append(f"{role}: GPIO{pin} outside ESP32-S3 board range 0..48")
        owners.setdefault(pin, []).append(role)
    for pin, roles in owners.items():
        if len(roles) > 1 and pin not in shared:
            errors.append(f"GPIO{pin} has conflicting primary owners: {', '.join(roles)}")

    sd = spec["sd"]
    if sd.get("transport") != "sdmmc" or int(sd.get("bus_width", 0)) != 4:
        errors.append("Murphy M4 SD must remain native 4-bit SDMMC")
    if sd.get("power_active_high") is not False:
        errors.append("Murphy M4 SD power gate is active-low")

    touch = spec["touch"]
    if touch.get("power_active_high") is not False:
        errors.append("Murphy M4 touch power gate is active-low")
    if int(touch.get("i2c_address", -1)) != 0x2E:
        errors.append("Murphy M4 touch address must be 0x2e")

    buses = spec.get("gpio_shared_groups", {})
    for dev in spec.get("physical_i2c_devices", []):
        bus = dev["bus"]
        if bus not in buses:
            errors.append(f"I2C device {dev['name']} references undeclared bus {bus}")
            continue
        expected = {int(dev["sda"]), int(dev["scl"])}
        actual = {int(x) for x in buses[bus]}
        if expected != actual:
            errors.append(
                f"I2C device {dev['name']} pins {sorted(expected)} disagree with {bus} {sorted(actual)}"
            )

    flash = spec["flash"]
    if int(flash.get("firmware_model_bytes", 0)) != 16 * 1024 * 1024:
        errors.append("firmware flash model must stay 16MiB until the executable target changes")
    # This is a known evidence conflict, not a typo to be 'cleaned up'.  Removing
    # it would destroy provenance and make the emulator look more certain than it is.
    if flash.get("status") != "discrepancy":
        errors.append("schematic-vs-firmware flash-size discrepancy must remain explicit")
    if int(flash.get("schematic_marking_nominal_bytes", 0)) == int(
        flash.get("firmware_model_bytes", 0)
    ):
        errors.append("flash discrepancy marker is internally inconsistent")

    return errors


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("spec", nargs="?", default="board/murphy_m4.json")
    args = p.parse_args(argv)
    path = Path(args.spec)
    try:
        errors = validate(load(path))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"board spec error: {exc}", file=sys.stderr)
        return 2
    if errors:
        for e in errors:
            print(f"ERROR: {e}")
        return 1
    print(f"PASS board spec: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
