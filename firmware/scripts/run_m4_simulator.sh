#!/usr/bin/env bash
# Murphy M4 native simulator launcher (no absolute machine paths).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build/simulator"
BIN="${BUILD}/m4_simulator"

# Prefer full Xcode toolchain when Command Line Tools libc++ is broken
# (missing __builtin_ctzg/__builtin_clzg). Fall back to default cmake.
cmake_configure() {
  local args=(-S "${ROOT}/simulator" -B "${BUILD}")
  if [[ -x /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++ ]]; then
    local XCODE_DEV=/Applications/Xcode.app/Contents/Developer
    local SDK="${XCODE_DEV}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    if [[ -d "${SDK}" ]]; then
      args+=(
        -DCMAKE_CXX_COMPILER="${XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++"
        -DCMAKE_C_COMPILER="${XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"
        -DCMAKE_OSX_SYSROOT="${SDK}"
      )
    fi
  elif command -v g++-14 >/dev/null 2>&1; then
    args+=(-DCMAKE_CXX_COMPILER="$(command -v g++-14)" -DCMAKE_C_COMPILER="$(command -v gcc-14)")
  fi
  cmake "${args[@]}"
}

if [[ ! -x "${BIN}" ]]; then
  echo "[run_m4_simulator] configuring/building simulator..."
  cmake_configure
  cmake --build "${BUILD}" -j
fi

exec "${BIN}" "$@"
