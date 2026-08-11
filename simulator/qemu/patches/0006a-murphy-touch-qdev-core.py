#!/usr/bin/env python3
"""Add qdev GPIO API declarations to the generated Murphy touch device."""
from pathlib import Path
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    path = Path(argv[1]).resolve() / "hw/input/murphy_ft6x36.c"
    text = path.read_text(encoding="utf-8")
    old = '#include "hw/qdev-properties.h"\n#include "qemu/module.h"'
    new = '#include "hw/qdev-properties.h"\n#include "hw/qdev-core.h"\n#include "qemu/module.h"'
    if text.count(old) != 1:
        raise RuntimeError("expected Murphy touch include block exactly once")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
