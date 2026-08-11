#!/usr/bin/env python3
"""
Safe APP1-only flash / rollback helper for Murphy M4.

This is NOT PlatformIO `pio run -t upload`. Generic PlatformIO upload may rewrite
the bootloader, partition table, and/or otadata.

What this tool does (when not --dry-run):
  1. Resolve and validate esptool + otatool BEFORE any flash write.
  2. Preflight: read partition table @ 0x8000 and verify factory dual-OTA shape.
  3. Optionally back up current APP1 and otadata.
  4. Write firmware.bin only at absolute offset 0x6e0000 (APP1).
  5. Select OTA slot 1 via otatool; fail the run if switch fails.

Rollback to factory APP0:
  python3 scripts/murphy_m4_app1_flash.py --port /dev/cu.usbmodemXXXX --select-slot 0

Restoring a previous APP1 binary does NOT select APP0. Rollback requires either:
  - otatool switch_ota_partition --slot 0, or
  - restoring the saved otadata backup from this helper.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

PART_TABLE_OFF = 0x8000
PART_TABLE_SIZE = 0xC00
APP0_OFF = 0x10000
APP0_SIZE = 0x6D0000
APP1_OFF = 0x6E0000
APP1_SIZE = 0x6D0000
OTADATA_OFF = 0xE000
OTADATA_SIZE = 0x2000


class FlashError(Exception):
    pass


def die(msg: str, code: int = 1) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_esptool() -> str:
    for cand in ("esptool.py", "esptool"):
        path = shutil.which(cand)
        if path:
            return path
    pio = Path.home() / ".platformio" / "penv" / "bin" / "esptool.py"
    if pio.exists():
        return str(pio)
    raise FlashError("esptool.py not found on PATH")


def find_otatool() -> str:
    """Resolve otatool.py from PATH, $IDF_PATH, or PlatformIO ESP-IDF package."""
    path = shutil.which("otatool.py")
    if path:
        return path
    idf = os.environ.get("IDF_PATH")
    if idf:
        for rel in (
            Path("components") / "app_update" / "otatool.py",
            Path("tools") / "otatool.py",
        ):
            cand = Path(idf) / rel
            if cand.exists():
                return str(cand)
    # PlatformIO ships framework-espidf with otatool even without a full IDF install.
    pio_pkg = Path.home() / ".platformio" / "packages" / "framework-espidf"
    for cand in (
        pio_pkg / "components" / "app_update" / "otatool.py",
        pio_pkg / "tools" / "otatool.py",
    ):
        if cand.exists():
            return str(cand)
    # Nested package layouts (versioned dirs).
    try:
        for hit in pio_pkg.rglob("otatool.py"):
            return str(hit)
    except OSError:
        pass
    raise FlashError(
        "otatool.py not found. Install ESP-IDF tools, set IDF_PATH, or install "
        "PlatformIO framework-espidf. Refusing to write APP1 without a way to "
        "select the OTA slot."
    )


def run(cmd: list[str], dry_run: bool) -> None:
    print("+", " ".join(cmd))
    if dry_run:
        return
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        raise FlashError(f"command failed ({e.returncode}): {' '.join(cmd)}") from e


def read_flash(esptool: str, port: str, offset: int, size: int, out: Path, dry_run: bool) -> None:
    run(
        [
            esptool,
            "--chip",
            "esp32s3",
            "--port",
            port,
            "read_flash",
            f"0x{offset:x}",
            f"0x{size:x}",
            str(out),
        ],
        dry_run,
    )


def write_flash(esptool: str, port: str, offset: int, image: Path, dry_run: bool) -> None:
    run(
        [
            esptool,
            "--chip",
            "esp32s3",
            "--port",
            port,
            "write_flash",
            "--flash_mode",
            "dio",
            "--flash_freq",
            "80m",
            "--flash_size",
            "16MB",
            f"0x{offset:x}",
            str(image),
        ],
        dry_run,
    )


def verify_partition_table(pt: bytes) -> None:
    if len(pt) < 64:
        raise FlashError("partition table too short")
    found_app0 = False
    found_app1 = False
    for off in range(0, min(len(pt), PART_TABLE_SIZE) - 32, 32):
        magic = struct.unpack_from("<H", pt, off)[0]
        if magic != 0x50AA:
            continue
        ptype = pt[off + 2]
        subtype = pt[off + 3]
        offset = struct.unpack_from("<I", pt, off + 4)[0]
        size = struct.unpack_from("<I", pt, off + 8)[0]
        if ptype == 0x00 and subtype == 0x10 and offset == APP0_OFF and size == APP0_SIZE:
            found_app0 = True
        if ptype == 0x00 and subtype == 0x11 and offset == APP1_OFF and size == APP1_SIZE:
            found_app1 = True
    if not (found_app0 and found_app1):
        raise FlashError(
            "partition table does not match factory M4 dual-OTA layout "
            f"(need app0@0x{APP0_OFF:x}/0x{APP0_SIZE:x} and app1@0x{APP1_OFF:x}/0x{APP1_SIZE:x}). "
            "Refusing to write."
        )
    print("Preflight OK: factory dual-OTA partition identity matches.")


def select_ota_slot(otatool: str, port: str, slot: int, dry_run: bool) -> None:
    if slot not in (0, 1):
        raise FlashError("slot must be 0 (APP0) or 1 (APP1)")
    run(
        [
            otatool,
            "--port",
            port,
            "switch_ota_partition",
            "--slot",
            str(slot),
        ],
        dry_run,
    )
    print(f"OTA slot {slot} selected via otatool.")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Murphy M4 safe APP1-only flash helper")
    ap.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem1101")
    ap.add_argument(
        "--firmware",
        default=".pio/build/murphy_m4/firmware.bin",
        help="Path to firmware.bin (APP1 payload)",
    )
    ap.add_argument("--dry-run", action="store_true", help="Print actions only; no device I/O")
    ap.add_argument("--backup-dir", default="backups/m4_app1", help="Directory for pre-write backups")
    ap.add_argument("--skip-backup", action="store_true", help="Skip APP1/otadata backup")
    ap.add_argument(
        "--select-slot",
        type=int,
        choices=[0, 1],
        default=None,
        help="Only switch OTA slot (0=APP0 rollback, 1=APP1) without writing firmware",
    )
    ap.add_argument(
        "--i-understand-app1-only",
        action="store_true",
        help="Required confirmation that this writes APP1 only at 0x6e0000",
    )
    args = ap.parse_args(argv)

    try:
        # Preflight tools BEFORE any write.
        esptool = find_esptool()
        otatool = find_otatool()
        print(f"esptool={esptool}")
        print(f"otatool={otatool}")
        print("Policy: never write bootloader, partition table, NVS, or APP0.")

        if args.select_slot is not None:
            select_ota_slot(otatool, args.port, args.select_slot, args.dry_run)
            return 0

        if not args.dry_run and not args.i_understand_app1_only:
            raise FlashError("Refusing to write without --i-understand-app1-only (or use --dry-run)")

        fw = Path(args.firmware)
        if not fw.exists() and not args.dry_run:
            raise FlashError(f"firmware not found: {fw}")
        if fw.exists():
            size = fw.stat().st_size
            print(f"firmware={fw} size={size} sha256={sha256_file(fw)}")
            if size > APP1_SIZE:
                raise FlashError(f"firmware {size} exceeds APP1 capacity {APP1_SIZE}")
        else:
            print(f"firmware={fw} (missing; dry-run only)")

        backup_dir = Path(args.backup_dir)
        if not args.dry_run:
            backup_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")

        pt_path = backup_dir / f"partitions_{stamp}.bin"
        read_flash(esptool, args.port, PART_TABLE_OFF, PART_TABLE_SIZE, pt_path, args.dry_run)
        if not args.dry_run:
            verify_partition_table(pt_path.read_bytes())
        else:
            print("dry-run: would verify partition table against factory dual-OTA layout")

        if not args.skip_backup:
            app1_bak = backup_dir / f"app1_before_{stamp}.bin"
            ota_bak = backup_dir / f"otadata_before_{stamp}.bin"
            print(f"Backing up APP1 -> {app1_bak}")
            read_flash(esptool, args.port, APP1_OFF, APP1_SIZE, app1_bak, args.dry_run)
            print(f"Backing up otadata -> {ota_bak}")
            read_flash(esptool, args.port, OTADATA_OFF, OTADATA_SIZE, ota_bak, args.dry_run)

        print(f"Writing APP1 only at 0x{APP1_OFF:x} (size limit 0x{APP1_SIZE:x})")
        write_flash(esptool, args.port, APP1_OFF, fw, args.dry_run)
        print("Selecting OTA slot 1 (APP1)")
        select_ota_slot(otatool, args.port, 1, args.dry_run)
        print("Done. Power-cycle or reset the device.")
        print("Rollback: --select-slot 0  (or restore the saved otadata backup)")
        return 0
    except FlashError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
