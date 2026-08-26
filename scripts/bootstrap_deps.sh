#!/usr/bin/env bash
# Reconstruct unvendored FreeInk SDK + third-party libs from one pinned archive.
# Run from repo root: bash scripts/bootstrap_deps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="${ROOT}/firmware"
M4_DEVICE_SHA="f86b134"
URL="https://github.com/einklover/m4-device/archive/${M4_DEVICE_SHA}.tar.gz"
TMP="${TMPDIR:-/tmp}/m4-device-bootstrap-$$"

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

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

fail() {
  echo "ERROR: $*" >&2
  echo "       required archive: einklover/m4-device@${M4_DEVICE_SHA}" >&2
  echo "       check network access, curl, tar, and the archive contents" >&2
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
if ((${#missing[@]} == 0)); then
  patch_qemu_input_manager
  echo "==> M4 dependencies already reconstructed from einklover/m4-device@${M4_DEVICE_SHA}; no download needed"
  exit 0
fi

command -v curl >/dev/null 2>&1 || fail "curl is required to fetch ${URL}"
command -v tar >/dev/null 2>&1 || fail "tar is required to unpack ${URL}"

echo "==> fetch einklover/m4-device@${M4_DEVICE_SHA}"
mkdir -p "$TMP"
if ! curl -fL --retry 3 --retry-delay 2 "$URL" -o "$TMP/m4-device.tar.gz"; then
  fail "could not download pinned archive ${URL}"
fi
mkdir -p "$TMP/src"
if ! tar -xzf "$TMP/m4-device.tar.gz" -C "$TMP/src" --strip-components=1; then
  fail "could not unpack pinned archive ${URL}"
fi
DEVICE="$TMP/src"

for path in "${REQUIRED_SENTINELS[@]}"; do
  [[ -f "$DEVICE/$path" ]] || fail "pinned archive is missing ${path}"
done

echo "==> install open-m4-sdk"
rm -rf "$FW/open-m4-sdk"
cp -a "$DEVICE/open-m4-sdk" "$FW/open-m4-sdk"
patch_qemu_input_manager

echo "==> install src/network/updater_fw.bin"
mkdir -p "$FW/src/network"
cp -a "$DEVICE/src/network/updater_fw.bin" "$FW/src/network/updater_fw.bin"

echo "==> install lib/{Epub,expat,miniz,picojpeg,Lua,EpdFont/builtinFonts}"
rm -rf \
  "$FW/lib/Epub" "$FW/lib/expat" "$FW/lib/miniz" \
  "$FW/lib/picojpeg" "$FW/lib/Lua" "$FW/lib/EpdFont/builtinFonts"
mkdir -p "$FW/lib/EpdFont"
cp -a "$DEVICE/lib/Epub" "$FW/lib/Epub"
cp -a "$DEVICE/lib/expat" "$FW/lib/expat"
cp -a "$DEVICE/lib/miniz" "$FW/lib/miniz"
cp -a "$DEVICE/lib/picojpeg" "$FW/lib/picojpeg"
cp -a "$DEVICE/lib/Lua" "$FW/lib/Lua"
cp -a "$DEVICE/lib/EpdFont/builtinFonts" "$FW/lib/EpdFont/builtinFonts"

missing=()
for path in "${REQUIRED_SENTINELS[@]}"; do
  [[ -f "$FW/$path" ]] || missing+=("$path")
done
if ((${#missing[@]} != 0)); then
  fail "bootstrap completed but reconstructed dependencies are missing: ${missing[*]}"
fi

echo "==> bootstrap OK: einklover/m4-device@${M4_DEVICE_SHA}"
echo "    next: cd firmware && pio run -e murphy_m4"
echo "    sim:  cd simulator && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure"
