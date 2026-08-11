#!/usr/bin/env python3
"""Compose a 16 MiB Murphy M4 ESP32-S3 flash image for Espressif QEMU.

The Murphy M4 firmware is built as an OTA app in app1 at 0x6e0000. A fresh
QEMU flash has no trustworthy OTA metadata, so the default ``auto`` mode mirrors
the same application binary into app0 and app1 when no factory/base flash dump
is supplied. If a real 16 MiB base flash dump is supplied, ``auto`` overlays
only app1 and preserves the factory bootloader, app0, NVS, OTA data and SPIFFS.

This tool is deliberately byte-oriented and does not depend on ESP-IDF or
esptool, which makes image composition deterministic and testable in native CI.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
import sys
from typing import Iterable

FLASH_SIZE = 16 * 1024 * 1024
BOOTLOADER_OFFSET = 0x000000
PARTITION_TABLE_OFFSET = 0x008000
OTA_DATA_OFFSET = 0x00E000
APP0_OFFSET = 0x010000
APP1_OFFSET = 0x6E0000
APP_SLOT_SIZE = 0x6D0000

_BUILD_CANDIDATES = {
    "bootloader": ("bootloader.bin",),
    "partitions": ("partitions.bin", "partition-table.bin"),
    "firmware": ("firmware.bin",),
    "ota_data": ("boot_app0.bin", "ota_data_initial.bin"),
}


class ImageError(RuntimeError):
    pass


@dataclass(frozen=True)
class Segment:
    name: str
    offset: int
    path: Path
    size: int
    sha256: str


def sha256_bytes(data: bytes | bytearray) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_int(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer/offset: {text}") from exc


def _resolve_file(
    explicit: str | None,
    build_dir: Path | None,
    key: str,
    required: bool,
) -> Path | None:
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.is_file():
            raise ImageError(f"{key} file not found: {path}")
        return path
    if build_dir is not None:
        for candidate in _BUILD_CANDIDATES[key]:
            path = (build_dir / candidate).resolve()
            if path.is_file():
                return path
    if required:
        where = f" under {build_dir}" if build_dir is not None else ""
        raise ImageError(
            f"missing required {key}{where}; pass --{key.replace('_', '-')}"
        )
    return None


def _load_base(base_flash: Path | None) -> bytearray:
    if base_flash is None:
        return bytearray(b"\xFF") * FLASH_SIZE
    data = base_flash.read_bytes()
    if len(data) != FLASH_SIZE:
        raise ImageError(
            f"base flash must be exactly {FLASH_SIZE} bytes (16 MiB), "
            f"got {len(data)}: {base_flash}"
        )
    return bytearray(data)


def _write_segment(
    image: bytearray,
    occupied: list[tuple[int, int, str]],
    name: str,
    offset: int,
    path: Path,
) -> Segment:
    data = path.read_bytes()
    end = offset + len(data)
    if offset < 0 or end > FLASH_SIZE:
        raise ImageError(
            f"{name} does not fit 16 MiB flash: "
            f"offset=0x{offset:x} size=0x{len(data):x}"
        )
    for lo, hi, other in occupied:
        if offset < hi and end > lo:
            raise ImageError(
                f"{name} [0x{offset:x},0x{end:x}) overlaps "
                f"{other} [0x{lo:x},0x{hi:x})"
            )
    image[offset:end] = data
    occupied.append((offset, end, name))
    return Segment(name, offset, path, len(data), sha256_bytes(data))


def _parse_extra(spec: str) -> tuple[int, Path]:
    if "=" not in spec:
        raise ImageError(f"--extra must be OFFSET=PATH, got: {spec}")
    raw_offset, raw_path = spec.split("=", 1)
    offset = parse_int(raw_offset)
    path = Path(raw_path).expanduser().resolve()
    if not path.is_file():
        raise ImageError(f"extra segment not found: {path}")
    return offset, path


def compose_image(
    *,
    output: Path,
    bootloader: Path | None = None,
    partitions: Path | None = None,
    firmware: Path | None = None,
    ota_data: Path | None = None,
    base_flash: Path | None = None,
    slot_mode: str = "auto",
    extras: Iterable[tuple[int, Path]] = (),
    write_manifest: bool = True,
) -> dict:
    if slot_mode not in {"auto", "app0", "app1", "mirror"}:
        raise ImageError(f"unsupported slot mode: {slot_mode}")

    base_flash = base_flash.resolve() if base_flash is not None else None
    image = _load_base(base_flash)
    occupied: list[tuple[int, int, str]] = []
    segments: list[Segment] = []

    if slot_mode == "auto":
        effective_slot = "app1" if base_flash is not None else "mirror"
    else:
        effective_slot = slot_mode

    # With a real factory dump, bootloader/partition/OTA inputs are optional:
    # preserve those bytes unless the caller explicitly replaces them.
    need_boot_material = base_flash is None
    if bootloader is None and need_boot_material:
        raise ImageError("bootloader is required when --base-flash is not supplied")
    if partitions is None and need_boot_material:
        raise ImageError("partition table is required when --base-flash is not supplied")
    if firmware is None:
        raise ImageError("firmware is required")

    if bootloader is not None:
        segments.append(
            _write_segment(image, occupied, "bootloader", BOOTLOADER_OFFSET, bootloader)
        )
    if partitions is not None:
        segments.append(
            _write_segment(
                image,
                occupied,
                "partition_table",
                PARTITION_TABLE_OFFSET,
                partitions,
            )
        )
    if ota_data is not None:
        segments.append(
            _write_segment(image, occupied, "ota_data", OTA_DATA_OFFSET, ota_data)
        )

    firmware = firmware.resolve()
    fw_size = firmware.stat().st_size
    if fw_size > APP_SLOT_SIZE:
        raise ImageError(
            f"firmware is 0x{fw_size:x} bytes, larger than Murphy app slot "
            f"0x{APP_SLOT_SIZE:x}"
        )

    if effective_slot in {"app0", "mirror"}:
        segments.append(_write_segment(image, occupied, "app0", APP0_OFFSET, firmware))
    if effective_slot in {"app1", "mirror"}:
        segments.append(_write_segment(image, occupied, "app1", APP1_OFFSET, firmware))

    for index, (offset, path) in enumerate(extras):
        segments.append(_write_segment(image, occupied, f"extra{index}", offset, path))

    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)

    manifest = {
        "format": 1,
        "flash_size": FLASH_SIZE,
        "flash_size_hex": f"0x{FLASH_SIZE:x}",
        "slot_mode_requested": slot_mode,
        "slot_mode_effective": effective_slot,
        "base_flash": str(base_flash) if base_flash is not None else None,
        "base_flash_sha256": (
            sha256_bytes(base_flash.read_bytes()) if base_flash is not None else None
        ),
        "image": str(output),
        "image_sha256": sha256_bytes(image),
        "segments": [
            {
                "name": segment.name,
                "offset": segment.offset,
                "offset_hex": f"0x{segment.offset:x}",
                "size": segment.size,
                "size_hex": f"0x{segment.size:x}",
                "path": str(segment.path),
                "sha256": segment.sha256,
            }
            for segment in sorted(segments, key=lambda item: item.offset)
        ],
    }

    if write_manifest:
        manifest_path = output.with_suffix(output.suffix + ".json")
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        manifest["manifest"] = str(manifest_path.resolve())

    return manifest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        help="PlatformIO build dir, usually m4-firmware/.pio/build/murphy_m4",
    )
    parser.add_argument(
        "--bootloader",
        help="bootloader.bin (auto-discovered from --build-dir)",
    )
    parser.add_argument(
        "--partitions",
        help="partitions.bin (auto-discovered from --build-dir)",
    )
    parser.add_argument(
        "--firmware",
        help="firmware.bin (auto-discovered from --build-dir)",
    )
    parser.add_argument(
        "--ota-data",
        help="optional boot_app0.bin/ota_data_initial.bin written at 0xe000",
    )
    parser.add_argument(
        "--base-flash",
        help="optional exact 16 MiB Murphy flash dump to preserve factory data",
    )
    parser.add_argument(
        "--slot",
        choices=("auto", "app0", "app1", "mirror"),
        default="auto",
        help="auto=mirror on blank image, app1 when a base flash dump is supplied",
    )
    parser.add_argument(
        "--extra",
        action="append",
        default=[],
        metavar="OFFSET=PATH",
        help="overlay an additional flash segment; may be repeated",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="murphy-m4-qemu-flash.bin",
        help="output full 16 MiB flash image",
    )
    parser.add_argument(
        "--no-manifest",
        action="store_true",
        help="do not write sidecar JSON manifest",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        build_dir = Path(args.build_dir).expanduser().resolve() if args.build_dir else None
        if build_dir is not None and not build_dir.is_dir():
            raise ImageError(f"build dir not found: {build_dir}")

        bootloader = _resolve_file(args.bootloader, build_dir, "bootloader", required=False)
        partitions = _resolve_file(args.partitions, build_dir, "partitions", required=False)
        firmware = _resolve_file(args.firmware, build_dir, "firmware", required=True)
        ota_data = _resolve_file(args.ota_data, build_dir, "ota_data", required=False)
        base_flash = Path(args.base_flash).expanduser().resolve() if args.base_flash else None
        if base_flash is not None and not base_flash.is_file():
            raise ImageError(f"base flash not found: {base_flash}")

        if base_flash is None:
            if bootloader is None:
                bootloader = _resolve_file(None, build_dir, "bootloader", required=True)
            if partitions is None:
                partitions = _resolve_file(None, build_dir, "partitions", required=True)

        extras = [_parse_extra(spec) for spec in args.extra]
        manifest = compose_image(
            output=Path(args.output).expanduser().resolve(),
            bootloader=bootloader,
            partitions=partitions,
            firmware=firmware,
            ota_data=ota_data,
            base_flash=base_flash,
            slot_mode=args.slot,
            extras=extras,
            write_manifest=not args.no_manifest,
        )
    except ImageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"wrote {manifest['image']}")
    print(f"sha256 {manifest['image_sha256']}")
    print(f"slot mode {manifest['slot_mode_effective']}")
    for segment in manifest["segments"]:
        print(
            f"  {segment['name']:<16} {segment['offset_hex']:>10} "
            f"{segment['size_hex']:>10}  {segment['path']}"
        )
    if "manifest" in manifest:
        print(f"manifest {manifest['manifest']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
