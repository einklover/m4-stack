#!/usr/bin/env python3
"""Generate a real page-turn wipe animation from two reader screenshots.

Kindle-style wipe: the new page enters from the right edge and sweeps left.
Frame sequence (3 strips + endpoints):
  frame_000 = old page (full)
  frame_001 = right 1/3 new page, left 2/3 old page
  frame_002 = right 2/3 new page, left 1/3 old page
  frame_003 = new page (full)

Each frame is a 48000-byte physical 800x480 1bpp frame (0=black, 1=white),
ready for Waveform Lab SD animation.

Usage:
  python3 scripts/page_wipe_gen.py page1.pbm page2.pbm --out /tmp/wf_wipe
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from m4adb_lib.pbm import read_p4_raw  # noqa: E402

FRAME_BYTES = 48000
PHYS_W, PHYS_H = 800, 480
STRIPS = 3


def logical_to_physical(raw: bytes, log_w: int, log_h: int) -> bytearray:
    """Rotate logical portrait screenshot (480x800) to physical 800x480."""
    out = bytearray(FRAME_BYTES)
    for y in range(log_h):
        for x in range(log_w):
            bit = (raw[(y * ((log_w + 7) // 8)) + (x // 8)] >> (7 - (x % 8))) & 1
            X = log_h - 1 - y
            Y = x
            if 0 <= X < PHYS_W and 0 <= Y < PHYS_H:
                byte_idx = Y * (PHYS_W // 8) + (X // 8)
                if bit:
                    out[byte_idx] &= ~(1 << (7 - (X % 8)))  # black = 0
                else:
                    out[byte_idx] |= (1 << (7 - (X % 8)))  # white = 1
    return out


def compose_wipe(old: bytearray, new: bytearray, new_right_frac: float) -> bytearray:
    """Right `new_right_frac` of the frame = new page, left = old page."""
    edge = int(PHYS_W * (1.0 - new_right_frac))
    out = bytearray(FRAME_BYTES)
    for row in range(PHYS_H):
        src = row * (PHYS_W // 8)
        # Left: old page bytes.
        for bx in range(0, edge // 8):
            out[src + bx] = old[src + bx]
        # Right: new page bytes.
        for bx in range((edge + 7) // 8, PHYS_W // 8):
            out[src + bx] = new[src + bx]
        # Edge byte: per-bit.
        if 0 <= edge < PHYS_W:
            for k in range(edge, min(edge + 8, PHYS_W)):
                if (new[src + k // 8] >> (7 - (k % 8))) & 1:
                    out[src + k // 8] |= (1 << (7 - (k % 8)))
                else:
                    out[src + k // 8] &= ~(1 << (7 - (k % 8)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("p1")
    ap.add_argument("p2")
    ap.add_argument("--out", default="/tmp/wf_wipe")
    ap.add_argument("--strips", type=int, default=STRIPS)
    ap.add_argument("--reverse", action="store_true", help="also emit reversed wipe frames")
    args = ap.parse_args()

    w1, h1, raw1 = read_p4_raw(Path(args.p1))
    w2, h2, raw2 = read_p4_raw(Path(args.p2))
    if (w1, h1) != (w2, h2):
        raise SystemExit(f"size mismatch: {w1}x{h1} vs {w2}x{h2}")
    old = logical_to_physical(raw1, w1, h1)
    new = logical_to_physical(raw2, w1, h1)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("frame_*.bin"):
        stale.unlink()

    n = max(2, args.strips)
    frames = []
    frames.append(bytes(old))
    for i in range(1, n):
        frac = i / n
        frames.append(bytes(compose_wipe(old, new, frac)))
    frames.append(bytes(new))
    for idx, f in enumerate(frames):
        (out / f"frame_{idx:03d}.bin").write_bytes(f)
    print(f"{len(frames)} wipe frames -> {out}")


if __name__ == "__main__":
    main()
