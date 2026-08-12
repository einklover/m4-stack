#!/usr/bin/env python3
"""Run Stage-13 full E2E using an already-built Murphy QEMU binary.

This wrapper exists so the repository's established pull-request QEMU workflow
can execute the complete firmware/SD/TTF/TXT/plugin journey without needing a
new workflow definition.  It bootstraps only host-side CI dependencies, reuses
the QEMU binary built by build_patched_qemu.py, and delegates all guest checks
to run_full_e2e_ci.py.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BOOTSTRAP_ENV = "M4_STAGE13_PYTHON_BOOTSTRAPPED"


def run(cmd: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd or ROOT, check=True)


def venv_python(venv: Path) -> Path:
    """Return the interpreter path for the isolated Stage-13 host environment."""
    if os.name == "nt":
        return venv / "Scripts" / "python.exe"
    return venv / "bin" / "python"


def dependency_install_command(python: Path) -> list[str]:
    """Keep Stage-13 Python packages attached to the interpreter that uses them."""
    return [
        str(python), "-m", "pip", "install", "--disable-pip-version-check",
        "platformio", "pyserial", "fonttools",
    ]


def ensure_ci_dependencies() -> Path:
    """Install CI-only host dependencies and return an isolated Python."""
    required = ["mkfs.fat", "mcopy"]
    need_apt = any(shutil.which(x) is None for x in required)
    font_candidates = [
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
    ]
    if not any(p.is_file() for p in font_candidates):
        need_apt = True
    if need_apt:
        run(["sudo", "apt-get", "update"])
        run([
            "sudo", "apt-get", "install", "-y", "--no-install-recommends",
            "dosfstools", "mtools", "fonts-wqy-zenhei", "python3-pip", "python3-venv",
        ])

    # GitHub's Ubuntu runner may have the user site disabled for the current
    # process.  A successful `pip --user` therefore does not guarantee that a
    # subsequent `import fontTools` in this process can see the package.  Build
    # one tiny disposable venv and re-exec the E2E runner through it instead.
    runner_temp = Path(os.environ.get("RUNNER_TEMP", "/tmp"))
    venv = runner_temp / "m4-stage13-python"
    if venv.exists():
        shutil.rmtree(venv)
    run([sys.executable, "-m", "venv", str(venv)])
    python = venv_python(venv)
    run(dependency_install_command(python))
    run([
        str(python), "-c",
        "import fontTools, platformio, serial; print('Stage13 Python bootstrap OK')",
    ])

    os.environ["PATH"] = str(python.parent) + os.pathsep + os.environ.get("PATH", "")
    if shutil.which("pio") is None:
        raise RuntimeError("PlatformIO CLI not available in Stage-13 venv")

    # The checked-out firmware intentionally vendors no generated external deps.
    run(["bash", "scripts/bootstrap_deps.sh"], cwd=ROOT)
    return python


def run_in_bootstrap_python(python: Path, qemu: Path) -> int:
    """Re-enter this runner with the exact interpreter owning CI packages."""
    env = os.environ.copy()
    env[BOOTSTRAP_ENV] = "1"
    env["PATH"] = str(python.parent) + os.pathsep + env.get("PATH", "")
    cmd = [str(python), str(Path(__file__).resolve()), str(qemu)]
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=ROOT, env=env, check=False).returncode


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("qemu", help="already-built qemu-system-xtensa")
    args = p.parse_args(argv)
    qemu = Path(args.qemu).expanduser().resolve()
    if not qemu.is_file():
        print(f"error: QEMU binary not found: {qemu}", file=sys.stderr)
        return 2

    try:
        if os.environ.get(BOOTSTRAP_ENV) != "1":
            python = ensure_ci_dependencies()
            return run_in_bootstrap_python(python, qemu)

        sys.path.insert(0, str(HERE))
        import run_full_e2e_ci as e2e

        # Clean only the Stage-13 scratch area. The QEMU source/build passed in
        # above lives elsewhere and is deliberately reused.
        if e2e.TMP.exists():
            shutil.rmtree(e2e.TMP)
        e2e.ART.mkdir(parents=True, exist_ok=True)
        _, _, sd = e2e.prepare_fixture()
        e2e.patch_e2e_control_surface()
        flash = e2e.build_firmware_and_flash()
        plugins = e2e.clone_plugins()
        e2e.boot_and_drive(qemu, flash, sd, plugins)
        (e2e.ART / "RESULT.txt").write_text(
            "PASS full-chain TTF TXT plugins (reused patched QEMU)\n", encoding="utf-8"
        )
        print("PASS full-chain TTF TXT plugins (reused patched QEMU)")
        return 0
    except Exception as exc:
        try:
            sys.path.insert(0, str(HERE))
            import run_full_e2e_ci as e2e
            e2e.ART.mkdir(parents=True, exist_ok=True)
            (e2e.ART / "RESULT.txt").write_text(
                f"FAIL {type(exc).__name__}: {exc}\n", encoding="utf-8"
            )
        except Exception:
            pass
        print(f"FAIL {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
