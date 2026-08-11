#!/usr/bin/env python3
"""Run an unmodified Murphy M4 production flash image in ESP32-S3 QEMU.

This is the long-term acceptance path for binary-level Murphy emulation. It is
intentionally different from ``run_murphy_bin.py``:

* input is a complete 16 MiB production/factory flash image;
* Octal PSRAM is always requested (shipping N16R8 contract);
* no ``M4_QEMU_BUILD`` firmware profile is assumed;
* watchdogs stay enabled unless explicitly disabled for diagnosis;
* ESP32-S3 QEMU eFuse and raw SD-card backing files can be supplied;
* Murphy idle GPIO inputs are seeded at the GPIO device, not faked in firmware;
* open_eth is opt-in because it is not the ESP32-S3 Wi-Fi peripheral.

Espressif QEMU already instantiates a DesignWare SD/MMC controller in the
ESP32-S3 SoC and attaches an ``if=sd`` drive to its SD bus. Board-level SD power
and card-detect wiring are separate Murphy board-model concerns.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import signal
import subprocess
import sys
import time

from run_murphy_bin import FLASH_SIZE, RunnerError, find_qemu

GPIO_DRIVER = "esp32s3.gpio"
PSRAM_DRIVER = "ssi_psram"
WDT_DRIVER = "timer.esp32c3.timg"
EFUSE_DRIVER = "nvram.esp32s3.efuse"
EFUSE_DRIVE_ID = "efuse"

# Active-low Murphy keys (GPIO0/1/2) and the FT6x36-style touch IRQ line
# (GPIO44, idle high) must not float low at boot. Other board lines remain low
# until the dedicated board device drives them.
MURPHY_IDLE_GPIO_INPUTS = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 44)


def existing_file(value: str | None, label: str) -> Path | None:
    if value is None:
        return None
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise RunnerError(f"{label} not found: {path}")
    if path.stat().st_size == 0:
        raise RunnerError(f"{label} is empty: {path}")
    return path


def validate_sd_image(path: Path | None) -> Path | None:
    if path is None:
        return None
    size = path.stat().st_size
    if size % 512:
        raise RunnerError(f"SD image size must be a multiple of 512 bytes, got {size}")
    return path


def build_cmd(
    qemu: str,
    flash: Path,
    *,
    psram_mb: int,
    gpio_input_default: int,
    efuse_file: Path | None,
    sd_image: Path | None,
    sd_read_only: bool,
    serial_file: Path | None,
    gdb: bool,
    disable_wdt: bool,
    open_eth: bool,
    extra: list[str],
) -> list[str]:
    if gpio_input_default < 0 or gpio_input_default > 0xFFFFFFFFFFFFFFFF:
        raise RunnerError("GPIO input-default mask must fit in 64 bits")

    cmd = [
        qemu,
        "-nographic",
        "-machine",
        "esp32s3",
        "-m",
        f"{psram_mb}M",
        "-drive",
        f"file={flash},if=mtd,format=raw",
        "-global",
        f"driver={GPIO_DRIVER},property=strap_mode,value=0x04",
        "-global",
        f"driver={GPIO_DRIVER},property=input-default,value=0x{gpio_input_default:x}",
        # Shipping Murphy M4 uses Octal/OPI PSRAM. Do not silently switch this
        # acceptance path to Quad PSRAM: an Octal failure belongs below guest.
        "-global",
        f"driver={PSRAM_DRIVER},property=is_octal,value=true",
    ]

    if efuse_file is not None:
        cmd += [
            "-drive",
            f"file={efuse_file},if=none,format=raw,id={EFUSE_DRIVE_ID}",
            "-global",
            f"driver={EFUSE_DRIVER},property=drive,value={EFUSE_DRIVE_ID}",
        ]

    if sd_image is not None:
        drive = f"file={sd_image},if=sd,format=raw"
        if sd_read_only:
            drive += ",readonly=on"
        cmd += ["-drive", drive]

    if disable_wdt:
        cmd += [
            "-global",
            f"driver={WDT_DRIVER},property=wdt_disable,value=true",
        ]

    if open_eth:
        cmd += ["-nic", "user,model=open_eth"]

    if serial_file is not None:
        cmd += ["-serial", f"file:{serial_file}"]
    else:
        cmd += ["-serial", "mon:stdio"]

    if gdb:
        cmd += ["-gdb", "tcp::3333", "-S"]

    cmd += extra
    return cmd


def run_for(cmd: list[str], seconds: float, stdout_log: Path | None) -> int:
    print("+", " ".join(cmd), flush=True)
    out = open(stdout_log, "w", encoding="utf-8") if stdout_log else None
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=out if out else None,
            stderr=subprocess.STDOUT if out else None,
        )
        deadline = time.monotonic() + seconds
        try:
            while proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.1)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
        return proc.returncode or 0
    finally:
        if out:
            out.close()


def run_probe(serial: Path) -> int:
    probe = Path(__file__).resolve().parent / "probe_boot.py"
    output = serial.with_suffix(serial.suffix + ".probe.json")
    rc = subprocess.call([sys.executable, str(probe), str(serial), "-o", str(output)])
    if output.is_file():
        data = json.loads(output.read_text(encoding="utf-8"))
        print("production-bin probe:")
        print(f"  highest_stage={data.get('highest_stage')}")
        print(f"  checkpoint={data.get('highest_firmware_checkpoint')}")
        print(f"  failure_class={data.get('failure_class')}")
        for key, value in data.get("acceptance", {}).items():
            print(f"  {key}={value}")
    return rc


def parse_u64(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if parsed < 0 or parsed > 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in unsigned 64 bits")
    return parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash_image", help="complete 16 MiB production/factory flash image")
    parser.add_argument("--qemu", default=None, help="path to qemu-system-xtensa")
    parser.add_argument("--efuse-file", default=None, help="ESP32-S3 QEMU joint-format eFuse backing file")
    parser.add_argument("--sd-image", default=None, help="raw SD-card image attached to native ESP32-S3 SDMMC")
    parser.add_argument("--sd-read-only", action="store_true", help="open --sd-image read-only")
    parser.add_argument("--psram-mb", type=int, default=8, choices=(8, 16, 32))
    parser.add_argument(
        "--gpio-input-default",
        type=parse_u64,
        default=MURPHY_IDLE_GPIO_INPUTS,
        help="64-bit external GPIO idle-level mask (default: Murphy keys/touch IRQ high)",
    )
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--serial-file", default=None, help="guest UART output file")
    parser.add_argument("--log", default=None, help="QEMU process stdout/stderr log")
    parser.add_argument("--probe", action="store_true", help="classify the guest UART log")
    parser.add_argument("--gdb", action="store_true", help="wait for GDB on tcp::3333")
    parser.add_argument(
        "--disable-wdt",
        action="store_true",
        help="diagnostic escape hatch; production-fidelity runs leave watchdogs enabled",
    )
    parser.add_argument(
        "--open-eth",
        action="store_true",
        help="attach QEMU open_eth NAT for diagnostics; this is not ESP32 Wi-Fi emulation",
    )
    parser.add_argument("--dry-run", action="store_true")
    args, unknown = parser.parse_known_args(argv)

    extra: list[str] = []
    if unknown:
        if unknown[0] != "--":
            parser.error("extra QEMU arguments must follow a standalone --")
        extra = unknown[1:]

    try:
        flash = existing_file(args.flash_image, "flash image")
        assert flash is not None
        if flash.stat().st_size != FLASH_SIZE:
            raise RunnerError(
                f"production flash must be exactly {FLASH_SIZE} bytes (16 MiB), got {flash.stat().st_size}"
            )
        efuse = existing_file(args.efuse_file, "eFuse file")
        sd_image = validate_sd_image(existing_file(args.sd_image, "SD image"))
        qemu = find_qemu(args.qemu)
    except RunnerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    serial = Path(args.serial_file).expanduser().resolve() if args.serial_file else None
    log = Path(args.log).expanduser().resolve() if args.log else None
    if args.probe and serial is None:
        serial = Path("/tmp/murphy-production.serial.log")

    for path in (serial, log):
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)

    try:
        cmd = build_cmd(
            qemu,
            flash,
            psram_mb=args.psram_mb,
            gpio_input_default=args.gpio_input_default,
            efuse_file=efuse,
            sd_image=sd_image,
            sd_read_only=args.sd_read_only,
            serial_file=serial,
            gdb=args.gdb,
            disable_wdt=args.disable_wdt,
            open_eth=args.open_eth,
            extra=extra,
        )
    except RunnerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.dry_run:
        print("+", " ".join(cmd))
        return 0

    rc = run_for(cmd, args.seconds, log)
    if serial is not None and serial.is_file():
        print(f"serial log: {serial} ({serial.stat().st_size} bytes)")
        text = serial.read_text(encoding="utf-8", errors="replace")
        if text:
            print(text[-4000:])
        if args.probe:
            probe_rc = run_probe(serial)
            if probe_rc:
                return probe_rc
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
