#!/usr/bin/env bash
# Reconstruct unvendored FreeInk SDK + third-party libs (same pin as CI).
# Run once after clone, from repo root:  bash scripts/bootstrap_deps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="${ROOT}/firmware"
# Pin must match firmware/.github/workflows/native-app-build.yml
M4_DEVICE_SHA="${M4_DEVICE_SHA:-f86b134}"
URL="https://github.com/einklover/m4-device/archive/${M4_DEVICE_SHA}.tar.gz"
TMP="${TMPDIR:-/tmp}/m4-device-bootstrap-$$"

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "==> fetch m4-device @ ${M4_DEVICE_SHA}"
mkdir -p "$TMP"
curl -fL --retry 3 --retry-delay 2 "$URL" -o "$TMP/m4-device.tar.gz"
mkdir -p "$TMP/src"
tar -xzf "$TMP/m4-device.tar.gz" -C "$TMP/src" --strip-components=1
DEVICE="$TMP/src"

test -d "$DEVICE/open-m4-sdk" || { echo "missing open-m4-sdk in archive"; exit 2; }
test -f "$DEVICE/src/network/updater_fw.bin" || { echo "missing updater_fw.bin in m4-device archive"; exit 2; }

echo "==> install open-m4-sdk"
rm -rf "$FW/open-m4-sdk"
cp -a "$DEVICE/open-m4-sdk" "$FW/open-m4-sdk"

# QEMU plugin-debug builds set M4_QEMU_PLUGIN_DEBUG=1. Without this skip, the
# Arduino I2C-ng path hammers the QEMU I2C model every loop and starves m4adb
# USB install chunking. Only active under that macro (production builds unchanged).
IM="$FW/open-m4-sdk/libs/hardware/InputManager/src/InputManager.cpp"
if [[ -f "$IM" ]] && ! grep -q 'M4_QEMU_PLUGIN_DEBUG' "$IM"; then
  echo "==> patch InputManager: skip touch poll under M4_QEMU_PLUGIN_DEBUG"
  python3 - "$IM" <<'PY'
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
fi

# Browser Bridge hygiene: snapshot-absolute stock HALF from a frozen frame.
# Pinned m4-device @ f86b134 already has waveformLabBaseline (FULL) but not
# waveformLabHygiene. Do not vendor open-m4-sdk; apply the same post-copy
# patch style as InputManager so CI / clean clones compile HalDisplay.
FID_H="$FW/open-m4-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h"
FID_CC="$FW/open-m4-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp"
if [[ -f "$FID_H" && -f "$FID_CC" ]] && ! grep -q 'waveformLabHygiene' "$FID_H"; then
  echo "==> patch FreeInkDisplay: snapshot HALF hygiene (waveformLabHygiene)"
  python3 - "$FID_H" "$FID_CC" <<'PY'
from pathlib import Path
import sys

hdr, src = Path(sys.argv[1]), Path(sys.argv[2])
h = hdr.read_text(encoding="utf-8")
c = src.read_text(encoding="utf-8")
old_h = """  // Waveform Lab baseline: absolute FULL refresh to the given frame.
  void waveformLabBaseline(const uint8_t* frame);
"""
new_h = """  // Waveform Lab baseline: absolute FULL refresh to the given frame.
  void waveformLabBaseline(const uint8_t* frame);
  // Snapshot-absolute stock HALF (0xD7 both-plane clean). Hygiene only.
  void waveformLabHygiene(const uint8_t* frame);
"""
old_c = """void FreeInkDisplay::waveformLabBaseline(const uint8_t* frame) {
  if (!_driver || !frame) return;
  syncPendingAsync();
  _shadowValid = false;
  _redRamSynced = false;
  // Absolute FULL: both planes rewritten from the frame (no diff against
  // whatever the panel currently shows).
  _driver->display(_bus, frame, nullptr, freeink::RefreshMode::Full, /*turnOff=*/false);
}
"""
new_c = """void FreeInkDisplay::waveformLabBaseline(const uint8_t* frame) {
  if (!_driver || !frame) return;
  syncPendingAsync();
  _shadowValid = false;
  _redRamSynced = false;
  // Absolute FULL: both planes rewritten from the frame (no diff against
  // whatever the panel currently shows).
  _driver->display(_bus, frame, nullptr, freeink::RefreshMode::Full, /*turnOff=*/false);
}

void FreeInkDisplay::waveformLabHygiene(const uint8_t* frame) {
  if (!_driver || !frame) return;
  syncPendingAsync();
  _shadowValid = false;
  _redRamSynced = false;
  // Absolute HALF from the caller snapshot (not the live HAL framebuffer).
  // Stock 0xD7 both-plane clean; seeds RED=BW. Not a substitute for FULL
  // FirstBaseline / Untrusted / Recover.
  _driver->display(_bus, frame, nullptr, freeink::RefreshMode::Half, /*turnOff=*/false);
}
"""
if old_h not in h:
    raise SystemExit(f"FreeInkDisplay.h waveformLabBaseline anchor missing in {hdr}")
if old_c not in c:
    raise SystemExit(f"FreeInkDisplay.cpp waveformLabBaseline anchor missing in {src}")
hdr.write_text(h.replace(old_h, new_h, 1), encoding="utf-8")
src.write_text(c.replace(old_c, new_c, 1), encoding="utf-8")
PY
fi

# platformio.ini embeds this helper image into every hardware build. It is an
# unvendored production artifact from the same pinned m4-device snapshot, not a
# generated placeholder; without restoring it a clean clone cannot build the
# normal murphy_m4 profile.
echo "==> install src/network/updater_fw.bin"
mkdir -p "$FW/src/network"
cp -a "$DEVICE/src/network/updater_fw.bin" "$FW/src/network/updater_fw.bin"

for src in lib/Epub lib/expat lib/miniz lib/picojpeg lib/Lua lib/EpdFont/builtinFonts; do
  test -d "$DEVICE/$src" || { echo "missing m4-device $src"; exit 2; }
done

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

for p in \
  open-m4-sdk/libs/hardware/BatteryMonitor/library.json \
  src/network/updater_fw.bin \
  lib/Epub/Epub.h \
  lib/Lua/src/lua.h \
  lib/expat/expat.h \
  lib/miniz/miniz.h \
  lib/picojpeg/picojpeg.h \
  lib/EpdFont/builtinFonts/all.h; do
  test -f "$FW/$p" || { echo "missing reconstructed dependency: $p"; exit 2; }
done

echo "==> bootstrap OK"
echo "    next: cd firmware && pio run -e murphy_m4"
echo "    sim:  cd simulator && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure"
