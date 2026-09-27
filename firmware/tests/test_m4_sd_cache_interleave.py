#!/usr/bin/env python3
"""Compile vendored SdFat FsCache and run the prepare/memcpy interleave."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDFAT = ROOT / "firmware" / ".pio" / "libdeps" / "murphy_m4" / "SdFat"
SRC = SDFAT / "src"
MAIN = Path(__file__).resolve().parent / "sd_cache_interleave_main.cpp"
CACHE = SRC / "common" / "FsCache.cpp"


def main() -> int:
    if not CACHE.is_file():
        print(f"missing vendored FsCache: {CACHE}", file=sys.stderr)
        return 2
    cmd = [
        "c++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DENABLE_ARDUINO_FEATURES=0",
        "-DUSE_BLOCK_DEVICE_INTERFACE=1",
        "-DSPI_DRIVER_SELECT=3",
        "-include",
        str(Path(__file__).resolve().parent / "sd_cache_host_prefix.h"),
        f"-I{SRC}",
        str(MAIN),
        str(CACHE),
        "-o",
        "/tmp/m4_sd_cache_interleave",
    ]
    print(" ".join(cmd))
    built = subprocess.run(cmd)
    if built.returncode != 0:
        return built.returncode
    ran = subprocess.run(["/tmp/m4_sd_cache_interleave"])
    return ran.returncode


if __name__ == "__main__":
    sys.exit(main())
