"""PBM (P4) helpers for logical-orientation screenshots."""

from __future__ import annotations

import hashlib
from pathlib import Path


def write_p4(path: Path, width: int, height: int, raw: bytes) -> None:
    row_bytes = (width + 7) // 8
    expected = row_bytes * height
    if len(raw) != expected:
        raise ValueError(f"raw size {len(raw)} != expected {expected}")
    path.parent.mkdir(parents=True, exist_ok=True)
    header = f"P4\n# m4adb logical screenshot\n{width} {height}\n".encode("ascii")
    path.write_bytes(header + raw)


def read_p4_raw(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P4"):
        raise ValueError("not a P4 PBM")
    # Parse header lines
    i = 0
    lines = []
    while i < len(data) and len(lines) < 10:
        j = data.find(b"\n", i)
        if j < 0:
            break
        line = data[i:j]
        i = j + 1
        if line.startswith(b"#") and len(lines) > 0:
            continue
        lines.append(line)
        if len(lines) >= 2 and lines[0] == b"P4":
            # After P4 and dimensions
            parts = lines[-1].split()
            if len(parts) >= 2:
                w, h = int(parts[0]), int(parts[1])
                raw = data[i:]
                return w, h, raw
    raise ValueError("invalid PBM header")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()
