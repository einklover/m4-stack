#!/usr/bin/env python3
"""Use the QEMU 9.x DeviceClass reset callback name for Murphy touch."""
from pathlib import Path
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    path = Path(argv[1]).resolve() / "hw/input/murphy_ft6x36.c"
    text = path.read_text(encoding="utf-8")
    old = "    dc->reset = murphy_ft6x36_reset;\n"
    new = "    dc->legacy_reset = murphy_ft6x36_reset;\n"
    if text.count(old) != 1:
        raise RuntimeError("expected exactly one Murphy touch DeviceClass reset assignment")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
