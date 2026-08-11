#!/usr/bin/env python3
"""Add qdev GPIO API declarations after the Stage 6 GPIO transform."""
from pathlib import Path
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return 2
    path = Path(argv[1]).resolve() / "hw/gpio/esp32_gpio.c"
    text = path.read_text(encoding="utf-8")
    old = '#include "hw/gpio/esp32_gpio.h"\n\n#define GPIO_OUT_OFF'
    new = '#include "hw/gpio/esp32_gpio.h"\n#include "hw/qdev-core.h"\n\n#define GPIO_OUT_OFF'
    if text.count(old) != 1:
        raise RuntimeError("expected transformed ESP32 GPIO include block exactly once")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
