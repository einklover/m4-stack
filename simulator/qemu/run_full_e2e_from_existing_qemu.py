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


def run(cmd: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd or ROOT, check=True)


def ensure_ci_dependencies() -> None:
    """Install the small set omitted by the legacy QEMU-only workflow."""
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
            "dosfstools", "mtools", "fonts-wqy-zenhei", "python3-pip",
        ])

    # Use user site to avoid mutating the runner image's system Python.
    run([sys.executable, "-m", "pip", "install", "--user", "platformio", "pyserial", "fonttools"])
    local_bin = str(Path.home() / ".local/bin")
    os.environ["PATH"] = local_bin + os.pathsep + os.environ.get("PATH", "")
    if shutil.which("pio") is None:
        raise RuntimeError("PlatformIO CLI not available after install")

    # The checked-out firmware intentionally vendors no generated external deps.
    run(["bash", "scripts/bootstrap_deps.sh"], cwd=ROOT)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("qemu", help="already-built qemu-system-xtensa")
    args = p.parse_args(argv)
    qemu = Path(args.qemu).expanduser().resolve()
    if not qemu.is_file():
        print(f"error: QEMU binary not found: {qemu}", file=sys.stderr)
        return 2

    try:
        ensure_ci_dependencies()
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
