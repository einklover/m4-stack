#!/usr/bin/env python3
"""Ensure reconstructed M4 dependencies exist before PlatformIO discovery."""

from pathlib import Path
import subprocess


REQUIRED_SENTINELS = (
    "open-m4-sdk/libs/hardware/BatteryMonitor/library.json",
    "open-m4-sdk/libs/hardware/InputManager/library.json",
    "open-m4-sdk/libs/display/FreeInkDisplay/library.json",
    "open-m4-sdk/libs/hardware/SDCardManager/library.json",
    "open-m4-sdk/libs/hardware/BoardConfig/library.json",
    "open-m4-sdk/libs/hardware/FrontlightManager/library.json",
    "open-m4-sdk/libs/hardware/PowerManager/library.json",
    "src/network/updater_fw.bin",
    "lib/Epub/Epub.h",
    "lib/Lua/src/lua.h",
    "lib/expat/expat.h",
    "lib/miniz/miniz.h",
    "lib/picojpeg/picojpeg.h",
    "lib/EpdFont/builtinFonts/all.h",
)


def missing_dependencies(firmware_dir: Path) -> list[str]:
    return [rel for rel in REQUIRED_SENTINELS if not (firmware_dir / rel).is_file()]


def ensure_dependencies(firmware_dir: Path, runner=subprocess.run) -> bool:
    missing = missing_dependencies(firmware_dir)
    if not missing:
        return False

    repo_root = firmware_dir.parent
    bootstrap = repo_root / "scripts" / "bootstrap_deps.sh"
    if not bootstrap.is_file():
        raise RuntimeError(f"missing bootstrap script: {bootstrap}")

    runner(["bash", str(bootstrap)], cwd=repo_root, check=True)
    remaining = missing_dependencies(firmware_dir)
    if remaining:
        raise RuntimeError(
            "bootstrap completed but dependencies are still missing: "
            + ", ".join(remaining)
        )
    return True


def _run_as_platformio_extra_script() -> None:
    # PlatformIO provides Import and env only while executing an extra script.
    # Keep those SCons globals out of module import so the contract can import
    # and exercise the helper with plain Python.
    if "Import" not in globals():
        return
    Import("env")
    ensure_dependencies(Path(env.subst("$PROJECT_DIR")))


_run_as_platformio_extra_script()
