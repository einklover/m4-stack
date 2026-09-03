#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
home = (root / "src/activities/home/HomeActivity.h").read_text(encoding="utf-8")

assert '#include "../../util/M4RuntimeMemory.h"' in home
assert 'm4LogRuntimeMemory("home-enter")' in home
print("m4 runtime memory integration contract: PASS")
