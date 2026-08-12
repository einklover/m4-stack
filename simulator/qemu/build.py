#!/usr/bin/env python3
"""Single source of truth for building Murphy-patched qemu-system-xtensa.

This is a thin, stable entry that always builds the **v3** patch series
(series-v3 + upstream.json). Do not introduce a second CI/local path that
targets a different series without updating this file and the docs together.

Usage::

  python3 simulator/qemu/build.py -j 6
  export QEMU_XTENSA=$HOME/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# Re-export the pinned v3 builder so local + CI share one entry.
from build_patched_qemu_v3 import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
