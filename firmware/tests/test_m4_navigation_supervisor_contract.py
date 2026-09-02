#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

NAV_FILES = [
    ROOT / "firmware/src/navigation/M4NavigationSupervisor.h",
    ROOT / "firmware/src/navigation/M4NavigationSupervisor.cpp",
]

missing = [str(path.relative_to(ROOT)) for path in NAV_FILES if not path.exists()]
if missing:
    raise SystemExit("P1D navigation supervisor RED contract: implementation missing:\n  " + "\n  ".join(missing))

source = "\n".join(path.read_text(encoding="utf-8") for path in NAV_FILES)

required = [
    "detach",
    "idempot",
    "callback",
]

missing_markers = [marker for marker in required if marker not in source.lower()]
if missing_markers:
    raise SystemExit(
        "P1D navigation supervisor RED contract missing detach safety markers:\n  "
        + "\n  ".join(missing_markers)
    )

print("m4 navigation supervisor contract: PASS")
