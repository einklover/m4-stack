#!/usr/bin/env python3
"""Create an empty raw FAT32 image for Murphy QEMU SDMMC experiments.

The output is intentionally a normal raw card image and can also be mounted or
written by host tools. A real SD-card dump is preferable when reproducing a
specific device state.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def format_image(path: Path, size_mb: int, label: str) -> None:
    if size_mb < 32:
        raise ValueError("FAT32 fixture must be at least 32 MiB")
    size = size_mb * 1024 * 1024
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.truncate(size)

    # ponytail: mtools first — pure user-space, no hdiutil attach (denied in
    # sandboxed macOS); explicit geometry because -f only knows preset sizes.
    mformat = shutil.which("mformat")
    if mformat:
        # QEMU requires the raw card file itself to stay an exact power of
        # two, so pick a CHS geometry that divides the size evenly and
        # re-truncate afterwards in case mformat resized the file.
        heads, spt = 64, 32
        tracks = max(1, (size // 512) // (heads * spt))
        subprocess.run(
            [mformat, "-i", str(path), "-C", "-F",
             "-t", str(tracks), "-h", str(heads), "-n", str(spt),
             "-v", label[:11], "::"],
            check=True,
        )
        with path.open("rb+") as f:
            f.truncate(size)
        return

    mkfs = shutil.which("mkfs.fat") or shutil.which("mkfs.vfat")
    if mkfs:
        subprocess.run([mkfs, "-F", "32", "-n", label[:11], str(path)], check=True)
        return

    newfs = shutil.which("newfs_msdos")
    if newfs:
        # macOS newfs_msdos wants a character device; attach the raw image first.
        attach = shutil.which("hdiutil")
        if attach:
            info = subprocess.run(
                [attach, "attach", "-imagekey", "diskimage-class=CRawDiskImage",
                 "-nomount", str(path)],
                check=True, capture_output=True, text=True,
            )
            dev = info.stdout.strip().split()[0]
            try:
                subprocess.run([newfs, "-F", "32", "-v", label[:11], dev], check=True)
            finally:
                subprocess.run([attach, "detach", dev], check=False,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        subprocess.run([newfs, "-F", "32", "-v", label[:11], str(path)], check=True)
        return

    path.unlink(missing_ok=True)
    raise RuntimeError(
        "no FAT formatter found; install dosfstools (mkfs.fat) or use macOS newfs_msdos"
    )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output")
    p.add_argument("--size-mb", type=int, default=64)
    p.add_argument("--label", default="M4SD")
    p.add_argument("--force", action="store_true")
    args = p.parse_args(argv)

    out = Path(args.output).expanduser().resolve()
    if out.exists() and not args.force:
        print(f"error: output exists (use --force): {out}", file=sys.stderr)
        return 2
    try:
        if out.exists():
            out.unlink()
        format_image(out, args.size_mb, args.label)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"created {out} ({out.stat().st_size // (1024 * 1024)} MiB FAT32)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
