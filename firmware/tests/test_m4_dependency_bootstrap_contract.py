#!/usr/bin/env python3
"""Regression contract for the in-tree M4 dependency validator."""

from importlib.util import module_from_spec, spec_from_file_location
import os
from pathlib import Path
import shutil
import stat
import subprocess
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "firmware" / "scripts" / "bootstrap_m4_deps.py"
PLATFORMIO = ROOT / "firmware" / "platformio.ini"
BOOTSTRAP = ROOT / "scripts" / "bootstrap_deps.sh"

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
)


def test_m4_pre_script_bootstraps_only_when_sentinels_are_missing() -> None:
    config = PLATFORMIO.read_text(encoding="utf-8")
    m4_base = config.split("[m4_base]", 1)[1].split("[env:murphy_m4_qemu]", 1)[0]
    assert "pre:scripts/bootstrap_m4_deps.py" in m4_base
    assert "lib_extra_dirs" in m4_base
    assert "file://open-m4-sdk/" not in m4_base
    assert "pre:scripts/bootstrap_m4_deps.py" not in config.split("[x4_base]", 1)[1].split("[x3_base]", 1)[0]
    assert "pre:scripts/bootstrap_m4_deps.py" not in config.split("[x3_base]", 1)[1].split("[env:x3_default]", 1)[0]
    assert SCRIPT.is_file()

    spec = spec_from_file_location("bootstrap_m4_deps", SCRIPT)
    assert spec and spec.loader
    module = module_from_spec(spec)
    spec.loader.exec_module(module)

    with TemporaryDirectory() as temp:
        repo = Path(temp) / "repo"
        firmware = repo / "firmware"
        firmware.mkdir(parents=True)
        bootstrap = repo / "scripts" / "bootstrap_deps.sh"
        bootstrap.parent.mkdir(parents=True)
        bootstrap.touch()
        calls = []

        def fake_runner(command, *, cwd, check):
            calls.append((command, cwd, check))
            for relative in REQUIRED_SENTINELS:
                path = firmware / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

        assert module.REQUIRED_SENTINELS == REQUIRED_SENTINELS
        assert module.missing_dependencies(firmware) == list(REQUIRED_SENTINELS)
        assert module.ensure_dependencies(firmware, runner=fake_runner) is True
        assert calls == [(["bash", str(repo / "scripts" / "bootstrap_deps.sh")], repo, True)]

        for relative in REQUIRED_SENTINELS:
            path = firmware / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

        calls.clear()
        assert module.missing_dependencies(firmware) == []
        assert module.ensure_dependencies(firmware, runner=fake_runner) is False
        assert calls == []


def test_root_bootstrap_repairs_qemu_patch_without_downloading_complete_tree() -> None:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    assert stat.S_IMODE(BOOTSTRAP.stat().st_mode) == 0o755
    patch_definition = source.index("patch_qemu_input_manager()")
    no_op = source.index("if ((${#missing[@]} != 0)); then")
    assert patch_definition < no_op
    assert source.count("patch_qemu_input_manager") == 2

    with TemporaryDirectory() as temp:
        root = Path(temp) / "repo"
        firmware = root / "firmware"
        firmware.mkdir(parents=True)
        scripts = root / "scripts"
        scripts.mkdir()
        script = scripts / "bootstrap_deps.sh"
        shutil.copy2(BOOTSTRAP, script)
        for relative in REQUIRED_SENTINELS:
            path = firmware / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()

        input_manager = firmware / "open-m4-sdk/libs/hardware/InputManager/src/InputManager.cpp"
        input_manager.parent.mkdir(parents=True, exist_ok=True)
        input_manager.write_text(
            """uint8_t InputManager::serviceTouch() {
#if FREEINK_CAP_TOUCH
  if (!touchDataEnabled) {
    return 0;
  }
""",
            encoding="utf-8",
        )
        fake_bin = Path(temp) / "bin"
        fake_bin.mkdir()
        fake_curl = fake_bin / "curl"
        fake_curl.write_text("#!/bin/sh\nexit 97\n", encoding="utf-8")
        fake_curl.chmod(0o755)
        env = os.environ.copy()
        env["PATH"] = f"{fake_bin}:{env['PATH']}"

        result = subprocess.run(
            ["bash", str(script)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        assert "fetch" not in result.stdout
        assert "M4_QEMU_PLUGIN_DEBUG" in input_manager.read_text(encoding="utf-8"), (
            result.stdout + result.stderr + "\n" + input_manager.read_text(encoding="utf-8")
        )


if __name__ == "__main__":
    test_m4_pre_script_bootstraps_only_when_sentinels_are_missing()
    test_root_bootstrap_repairs_qemu_patch_without_downloading_complete_tree()
    print("m4 dependency bootstrap contract: PASS")
