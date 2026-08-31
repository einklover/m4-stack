#!/usr/bin/env python3
"""Write a v4 RecentBooksStore recent.bin for Home validation (simulator SD only)."""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
from pathlib import Path

# Hero + three minis. Paths are fake m4cp URIs; production load does not require files.
BOOKS = [
    ("m4cp://fanqie/lizhi1", "长安的荔枝", "马伯庸", "", ""),
    ("m4cp://fanqie/santi1", "三体", "刘慈欣", "", ""),
    ("m4cp://fanqie/pingfan1", "平凡的世界", "路遥", "", ""),
    ("m4cp://fanqie/mingchao1", "明朝那些事儿", "当年明月", "", ""),
]


def write_string(buf: bytearray, s: str) -> None:
    raw = s.encode("utf-8")
    buf.extend(struct.pack("<I", len(raw)))
    buf.extend(raw)


def build_recent_bin() -> bytes:
    out = bytearray()
    out.append(4)  # RECENT_BOOKS_FILE_VERSION
    out.append(len(BOOKS))
    for path, title, author, cover, original in BOOKS:
        write_string(out, path)
        write_string(out, title)
        write_string(out, author)
        write_string(out, cover)
        write_string(out, original)
    return bytes(out)


def inject_sd(sd: Path, blob: bytes) -> None:
    mcopy = shutil.which("mcopy")
    mmd = shutil.which("mmd")
    if not mcopy or not mmd:
        raise SystemExit("mtools (mcopy/mmd) required to seed murphy-sd.img")
    tmp = sd.parent / "recent.bin"
    tmp.write_bytes(blob)
    subprocess.run([mmd, "-i", str(sd), "/.crosspoint"], check=False)
    subprocess.run([mcopy, "-o", "-i", str(sd), str(tmp), "::/.crosspoint/recent.bin"], check=True)
    print(f"seeded {len(BOOKS)} recents into {sd}")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--out", type=Path, default=None)
    p.add_argument("--sd", type=Path, default=None, help="FAT SD image to mcopy into")
    args = p.parse_args()
    blob = build_recent_bin()
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(blob)
        print(f"wrote {args.out} ({len(blob)} bytes)")
    if args.sd:
        inject_sd(args.sd, blob)
    if not args.out and not args.sd:
        Path("/tmp/m4sim/home-recent.bin").write_bytes(blob)
        print("wrote /tmp/m4sim/home-recent.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
