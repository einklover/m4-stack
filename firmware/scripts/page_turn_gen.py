#!/usr/bin/env python3
"""Generate kindle-style page-turn animation frames from two reader screenshots.

Input:  two m4adb logical PBM screenshots (480x800 portrait, P4 format).
Output: 800x480 physical frame sequence (frame_000.bin .. frame_NNN.bin),
        each 48000 bytes, ready for Waveform Lab SD animation.

The turn simulates a page sliding left under the finger: frame 0 = page A
(p1) full, frame N-1 = page B (p2) full.  Intermediates show page B revealed
left-to-right behind a moving page edge, with the uncovered region of page A
fading by waveform (frames only carry 1bpp; the intermediate gray illusion
comes from the LUT's partial drives, so we keep hard black/white content).

Usage:
  python3 scripts/page_turn_gen.py page1.pbm page2.pbm --out /tmp/wf_frames --count 16
"""
from __future__ import annotations

import argparse
from pathlib import Path

sys_path = None  # noqa
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from m4adb_lib.pbm import read_p4_raw  # noqa: E402

FRAME_BYTES = 48000  # 800x480 1bpp
PHYS_W, PHYS_H = 800, 480


def logical_to_physical(raw: bytes, log_w: int, log_h: int) -> bytearray:
    """Rotate the logical portrait screenshot (480x800) to physical 800x480.

    Logical (x,y) maps to physical (X,Y) with a 90° rotation:
      X = log_h - 1 - y, Y = x
    (orientation handled by the renderer; verify on device and flip if needed).

    Polarity: PBM 1 = black, but SSD1677 frame bits are 0 = black (0xFF = white),
    so black PBM pixels must be written as cleared (0) bits.
    """
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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("p1")
    ap.add_argument("p2")
    ap.add_argument("--out", default="/tmp/wf_frames")
    ap.add_argument("--count", type=int, default=16)
    args = ap.parse_args()

    w1, h1, raw1 = read_p4_raw(Path(args.p1))
    w2, h2, raw2 = read_p4_raw(Path(args.p2))
    if (w1, h1) != (w2, h2):
        raise SystemExit(f"size mismatch: {w1}x{h1} vs {w2}x{h2}")
    phys1 = logical_to_physical(raw1, w1, h1)
    phys2 = logical_to_physical(raw2, w1, h1)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    # Delete stale frames.
    for old in out.glob("frame_*.bin"):
        old.unlink()

    n = max(2, args.count)
    for i in range(n):
        t = i / (n - 1)
        # Page B revealed left-to-right; page A still visible on the right.
        edge = int(PHYS_W * t)
        frame = bytearray(FRAME_BYTES)
        for row in range(PHYS_H):
            src_off = row * (PHYS_W // 8)
            # Right side (unrevealed): page A content.
            for byte_x in range(edge // 8, PHYS_W // 8):
                frame[src_off + byte_x] = phys1[src_off + byte_x]
            # Left side (revealed): page B content.
            for byte_x in range(0, edge // 8):
                frame[src_off + byte_x] = phys2[src_off + byte_x]
            # Partial edge byte: blend by bit.
            if 0 <= edge < PHYS_W:
                bit_x = edge
                for k in range(bit_x, min(bit_x + 8, PHYS_W)):
                    b1 = (phys1[src_off + k // 8] >> (7 - (k % 8))) & 1
                    b2 = (phys2[src_off + k // 8] >> (7 - (k % 8))) & 1
                    if b2:
                        frame[src_off + k // 8] |= (1 << (7 - (k % 8)))
                    else:
                        frame[src_off + k // 8] &= ~(1 << (7 - (k % 8)))
        (out / f"frame_{i:03d}.bin").write_bytes(bytes(frame))

    # Also emit the two endpoints as standalone frames for direct A->B runs.
    (out / "frame_000.bin").write_bytes(bytes(phys1))
    (out / f"frame_{n - 1:03d}.bin").write_bytes(bytes(phys2))
    print(f"{n} frames written to {out}")


if __name__ == "__main__":
    main()
