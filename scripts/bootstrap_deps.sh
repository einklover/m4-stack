#!/usr/bin/env bash
# Verify in-tree FreeInk SDK + third-party libs (vendored in this monorepo).
# Run from repo root: bash scripts/bootstrap_deps.sh
#
# These trees used to be reconstructed from the private archive
# einklover/m4-device@f86b134. They are now committed under firmware/ so
# clean clones and third parties do not need that private repo.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="${ROOT}/firmware"

REQUIRED_SENTINELS=(
  "open-m4-sdk/libs/hardware/BatteryMonitor/library.json"
  "open-m4-sdk/libs/hardware/InputManager/library.json"
  "open-m4-sdk/libs/display/FreeInkDisplay/library.json"
  "open-m4-sdk/libs/hardware/SDCardManager/library.json"
  "open-m4-sdk/libs/hardware/BoardConfig/library.json"
  "open-m4-sdk/libs/hardware/FrontlightManager/library.json"
  "open-m4-sdk/libs/hardware/PowerManager/library.json"
  "src/network/updater_fw.bin"
  "lib/Epub/Epub.h"
  "lib/Lua/src/lua.h"
  "lib/expat/expat.h"
  "lib/miniz/miniz.h"
  "lib/picojpeg/picojpeg.h"
  "lib/EpdFont/builtinFonts/all.h"
)

fail() {
  echo "ERROR: $*" >&2
  echo "       M4 dependencies are vendored in-tree under firmware/." >&2
  echo "       Restore the missing paths from git; do not fetch private archives." >&2
  exit 1
}

patch_qemu_input_manager() {
  local im="$FW/open-m4-sdk/libs/hardware/InputManager/src/InputManager.cpp"
  if [[ ! -f "$im" ]] || grep -q 'M4_QEMU_PLUGIN_DEBUG' "$im"; then
    return
  fi

  command -v python3 >/dev/null 2>&1 || fail "python3 is required to preserve the QEMU InputManager patch"
  echo "==> patch InputManager: skip touch poll under M4_QEMU_PLUGIN_DEBUG"
  if ! python3 - "$im" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = """uint8_t InputManager::serviceTouch() {
#if FREEINK_CAP_TOUCH
  if (!touchDataEnabled) {
    return 0;
  }
"""
new = """uint8_t InputManager::serviceTouch() {
#if FREEINK_CAP_TOUCH
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  // QEMU I2C model + Arduino ng driver currently spam invalid opcodes and
  // starve the main loop (breaks m4adb USB install chunking). Skip touch.
  return 0;
#endif
  if (!touchDataEnabled) {
    return 0;
  }
"""
if old not in text:
    raise SystemExit(f"InputManager serviceTouch anchor missing in {path}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
  then
    fail "could not apply the QEMU InputManager patch to $im"
  fi
}

missing=()
for path in "${REQUIRED_SENTINELS[@]}"; do
  [[ -f "$FW/$path" ]] || missing+=("$path")
done
if ((${#missing[@]} != 0)); then
  fail "vendored M4 dependencies are missing: ${missing[*]}"
fi

patch_qemu_input_manager
echo "==> M4 dependencies present in-tree (vendored); no network fetch"
echo "    next: cd firmware && pio run -e murphy_m4"
echo "    sim:  cd simulator && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure"
