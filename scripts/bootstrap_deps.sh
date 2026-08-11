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
