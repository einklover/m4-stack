#!/usr/bin/env python3
"""Run a complete 16 MiB Murphy M4 flash image in Espressif ESP32-S3 QEMU.

This is the instruction/SoC layer, not a claim that every Murphy board
peripheral is already emulated. The supplied image is executed by Espressif's
ESP32-S3 QEMU; unsupported board devices can still stop firmware during HAL
initialization and are the next layer to model.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

FLASH_SIZE = 16 * 1024 * 1024
_REQUIRED_FLASH_CONFIG = "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"


class RunnerError(RuntimeError):
    pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash_image", help="full 16 MiB raw flash image")
    parser.add_argument("--idf-py", default="idf.py", help="idf.py executable")
    parser.add_argument(
        "--build-dir",
        default="build-fullflash",
        help="ESP-IDF build directory inside qemu/",
    )
    parser.add_argument("--gdb", action="store_true", help="start QEMU waiting for GDB")
    parser.add_argument(
        "--graphics",
        action="store_true",
        help="enable Espressif QEMU virtual framebuffer window",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="skip building the launcher project before starting QEMU",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print commands without executing them",
    )
    return parser


def validate_flash(path: Path) -> None:
    if not path.is_file():
        raise RunnerError(f"flash image not found: {path}")
    size = path.stat().st_size
    if size != FLASH_SIZE:
        raise RunnerError(
            f"QEMU flash image must be exactly {FLASH_SIZE} bytes (16 MiB); "
            f"got {size}: {path}"
        )


def validate_existing_sdkconfig(qemu_dir: Path) -> None:
    """Reject stale generated configs that override sdkconfig.defaults.

    ESP-IDF keeps the project-level ``sdkconfig`` across build directories. A
    user who ran the older smoke target may therefore still have a 2/4 MiB
    config even though this branch ships a 16 MiB ``sdkconfig.defaults``.
    """

    sdkconfig = qemu_dir / "sdkconfig"
    if not sdkconfig.is_file():
        return
    text = sdkconfig.read_text(encoding="utf-8", errors="replace")
    if _REQUIRED_FLASH_CONFIG not in text:
        raise RunnerError(
            "qemu/sdkconfig exists but is not configured for 16 MiB flash. "
            "Remove qemu/sdkconfig (and optionally qemu/build-fullflash), then "
            "rerun so sdkconfig.defaults can create a fresh Murphy config."
        )


def command_lines(
    *,
    idf_py: str,
    build_dir: str,
    flash_image: Path,
    gdb: bool,
    graphics: bool,
    no_build: bool,
) -> list[list[str]]:
    commands: list[list[str]] = []
    if not no_build:
        commands.append([idf_py, "-B", build_dir, "build"])

    qemu = [
        idf_py,
        "-B",
        build_dir,
        "qemu",
        "--flash-file",
        str(flash_image),
    ]
    if gdb:
        qemu.append("--gdb")
    if graphics:
        qemu.append("--graphics")
    qemu.append("monitor")
    commands.append(qemu)
    return commands


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    flash = Path(args.flash_image).expanduser().resolve()
    try:
        validate_flash(flash)
        qemu_dir = Path(__file__).resolve().parent
        validate_existing_sdkconfig(qemu_dir)

        if not args.dry_run and shutil.which(args.idf_py) is None:
            raise RunnerError(
                f"{args.idf_py!r} not found in PATH; export ESP-IDF first and "
                "install Espressif qemu-xtensa"
            )
        if not args.dry_run and "IDF_PATH" not in os.environ:
            raise RunnerError("IDF_PATH is not set; source ESP-IDF export.sh first")

        commands = command_lines(
            idf_py=args.idf_py,
            build_dir=args.build_dir,
            flash_image=flash,
            gdb=args.gdb,
            graphics=args.graphics,
            no_build=args.no_build,
        )
        for command in commands:
            print("+", shlex.join(command))
            if not args.dry_run:
                subprocess.run(command, cwd=qemu_dir, check=True)
    except (RunnerError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
