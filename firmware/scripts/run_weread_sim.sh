#!/usr/bin/env bash
# Run WeRead .m4x plugin in the native simulator (mock or live network).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

MODE="${1:-mock}"  # mock | live | headless
shift || true

BIN="${ROOT}/build/simulator/m4_simulator"
if [[ ! -x "${BIN}" ]]; then
  echo "[weread-sim] building simulator..."
  "${ROOT}/scripts/run_m4_simulator.sh" --help >/dev/null 2>&1 || true
  # force rebuild if launcher only printed help
  if [[ ! -x "${BIN}" ]]; then
    XCODE_DEV=/Applications/Xcode.app/Contents/Developer
    if [[ -x "${XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++" ]]; then
      cmake -S simulator -B build/simulator \
        -DCMAKE_CXX_COMPILER="${XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++" \
        -DCMAKE_C_COMPILER="${XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" \
        -DCMAKE_OSX_SYSROOT="${XCODE_DEV}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    else
      cmake -S simulator -B build/simulator
    fi
    cmake --build build/simulator -j
  fi
fi

APP="${ROOT}/test/fixtures/m4x/weread_src"
OUT="${ROOT}/build/simulator/out_weread"
SD="${ROOT}/test/fixtures/simulator/sdroot"

case "${MODE}" in
  headless)
    exec "${BIN}" --m4x "${APP}" --m4x-mock --headless --allow-writes \
      --sd-root "${SD}" --out "${OUT}" "$@"
    ;;
  mock)
    echo "[weread-sim] interactive MOCK (no real WeRead login)"
    echo "  click = touch · Enter = confirm · Esc = back · S = screenshot"
    exec "${BIN}" --m4x "${APP}" --m4x-mock --allow-writes \
      --sd-root "${SD}" --out "${OUT}" --scale 1 "$@"
    ;;
  live)
    echo "[weread-sim] interactive LIVE network (curl → weread.qq.com)"
    echo "  QR is a placeholder — copy the [M4xSim QR URL] from the terminal to phone."
    exec "${BIN}" --m4x "${APP}" --allow-writes \
      --sd-root "${SD}" --out "${OUT}" --scale 1 "$@"
    ;;
  *)
    echo "Usage: $0 [mock|live|headless] [extra m4_simulator flags...]"
    exit 2
    ;;
esac
