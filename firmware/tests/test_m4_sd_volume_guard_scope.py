#!/usr/bin/env python3
"""Static coverage of the SdFat volume-guard patch, plus the create-fail lock."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware"
PATCH = FIRMWARE / "scripts" / "patch_sdfat_volume_guard.py"
sys.path.insert(0, str(PATCH.parent))
import patch_sdfat_volume_guard as patcher  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        added = patcher.patch_tree(Path(tmp))
        if added != 0:
            print("empty libdeps must stay a soft skip", file=sys.stderr)
            return 1
    present = list((FIRMWARE / ".pio" / "libdeps").glob("*/SdFat"))
    if not present:
        print("no installed SdFat; scope assert skipped", file=sys.stderr)
        return 2
    first = patcher.patch_tree(FIRMWARE / ".pio" / "libdeps")
    second = patcher.patch_tree(FIRMWARE / ".pio" / "libdeps")
    if second != 0:
        print(f"second patch inserted {second}", file=sys.stderr)
        return 3
    print(f"volume guard scope ok first_insert={first} second_insert={second}")

    sdk = (FIRMWARE / "open-m4-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp").read_text()
    for body in sdk.split("bool SDCardManager::begin() {")[1:]:
        prep = body.find("m4SdVolumeLockPrepare()")
        dev = body.find("_dev->begin")
        spi = body.find("sd.begin")
        touch = min(x for x in (dev, spi) if x != -1)
        if prep < 0 or prep > touch:
            print("mutex prepare is not before the block device", file=sys.stderr)
            return 4
    lock = (FIRMWARE / "src/sd/M4SdVolumeLock.cpp").read_text()
    if "while (!gMu" in lock or "xSemaphoreCreateRecursiveMutex()" in lock:
        print("dynamic mutex busy-wait is still present", file=sys.stderr)
        return 5

    cmd = [
        "c++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        f"-I{FIRMWARE / 'include'}",
        f"-I{FIRMWARE / 'tests' / 'freertos_stub'}",
        str(FIRMWARE / "tests" / "sd_volume_lock_fail_main.cpp"),
        str(FIRMWARE / "src/sd/M4SdVolumeLock.cpp"),
        "-o",
        "/tmp/m4_sd_volume_lock_fail",
    ]
    print(" ".join(cmd))
    built = subprocess.run(cmd)
    if built.returncode != 0:
        return built.returncode
    ran = subprocess.run(["/tmp/m4_sd_volume_lock_fail"])
    if ran.returncode != 0:
        return ran.returncode
    print("volume lock create-fail ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
