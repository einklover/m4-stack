#!/usr/bin/env python3
"""Boot Murphy production-profile firmware in patched QEMU for plugin debug.

Pipeline:
  1. Build ``murphy_m4_qemu_plugin`` (UART0 Serial + m4adb auto-auth + open_eth).
  2. Compose 16 MiB flash + FAT32 SD image.
  3. Launch patched QEMU (``murphy-m4``, octal PSRAM, open_eth user net, serial PTY).
  4. Wait for m4adb ``ping``; optionally install/launch a plugin package.

Example::

  python3 simulator/qemu/build_patched_qemu_v2.py -j 6
  export QEMU_XTENSA=$HOME/.cache/murphy-m4/espressif-qemu-v2/build-murphy-v2/qemu-system-xtensa

  python3 simulator/qemu/run_plugin_debug.py \\
      --plugin-src /path/to/m4-fanqie-plugin \\
      --app-id com.fanqie.client \\
      --seconds 120

  # Interactive: leave QEMU up and use m4adb against the printed PTY
  python3 simulator/qemu/run_plugin_debug.py --keep-alive
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
TMP = Path(os.environ.get("M4_PLUGIN_DEBUG_TMP", "/tmp/m4-plugin-debug"))
ART = TMP / "artifacts"


def run(cmd: list[str], *, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess:
    print("+", " ".join(shlex.quote(str(x)) for x in cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd or ROOT, check=check)


def find_qemu(explicit: str | None) -> Path:
    candidates = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    env = os.environ.get("QEMU_XTENSA")
    if env:
        candidates.append(Path(env))
    candidates += [
        Path.home() / ".cache/murphy-m4/espressif-qemu-v2/build-murphy-v2/qemu-system-xtensa",
        Path.home() / ".cache/murphy-m4/espressif-qemu/build-murphy/qemu-system-xtensa",
        Path.home() / ".cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa",
    ]
    for c in candidates:
        if c.is_file():
            return c.resolve()
    raise SystemExit(
        "patched qemu-system-xtensa not found; run:\n"
        "  python3 simulator/qemu/build_patched_qemu_v2.py -j 6\n"
        "  export QEMU_XTENSA=$HOME/.cache/murphy-m4/espressif-qemu-v2/build-murphy-v2/qemu-system-xtensa"
    )


def build_firmware() -> Path:
    env = os.environ.copy()
    env["PATH"] = str(Path.home() / ".platformio/penv/bin") + os.pathsep + env.get("PATH", "")
    run(["pio", "run", "-e", "murphy_m4_qemu_plugin"], cwd=ROOT / "firmware")
    # PlatformIO still writes under env name
    bdir = ROOT / "firmware/.pio/build/murphy_m4_qemu_plugin"
    if not (bdir / "firmware.bin").is_file():
        raise SystemExit(f"missing firmware.bin under {bdir}")
    return bdir


def compose_flash(build_dir: Path) -> Path:
    ART.mkdir(parents=True, exist_ok=True)
    flash = ART / "murphy-plugin-16m.bin"
    run([
        sys.executable, str(ROOT / "simulator/tools/murphy_flash_image.py"),
        "--build-dir", str(build_dir), "-o", str(flash),
    ])
    return flash


def make_sd() -> Path:
    ART.mkdir(parents=True, exist_ok=True)
    sd = ART / "murphy-sd.img"
    run([
        sys.executable, str(HERE / "make_sd_image.py"),
        str(sd), "--size-mb", "64", "--force",
    ])
    return sd


def m4adb(pty: str, args: list[str], *, timeout: int = 180, check: bool = True) -> str:
    cmd = [
        sys.executable, str(ROOT / "firmware/scripts/m4adb.py"),
        "--port", pty, "--no-daemon", "--timeout", "25", *args,
    ]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    cp = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=timeout)
    print(cp.stdout, end="")
    if check and cp.returncode:
        raise RuntimeError(f"m4adb {' '.join(args)} failed rc={cp.returncode}")
    return cp.stdout


def boot(qemu: Path, flash: Path, sd: Path, *, serial_log: Path) -> tuple[subprocess.Popen, str]:
    qlog = ART / "qemu.log"
    cmd = [
        str(qemu), "-nographic", "-monitor", "none",
        "-machine", "murphy-m4", "-m", "8M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-drive", f"file={sd},if=sd,format=raw",
        "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
        "-global", "driver=esp32s3.gpio,property=input-default,value=0x100000000007",
        "-global", "driver=ssi_psram,property=is_octal,value=true",
        "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
        "-nic", "user,model=open_eth",
        "-serial", "pty",
    ]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    serial_log.parent.mkdir(parents=True, exist_ok=True)
    with qlog.open("w", encoding="utf-8") as lf:
        # tee serial is via pty; qemu announces char device on stdout/stderr
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT, text=True)

    pty = ""
    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited early rc={proc.returncode}; see {qlog}")
        text = qlog.read_text(encoding="utf-8", errors="replace")
        # Linux: /dev/pts/N ; macOS: /dev/ttysNNN
        hits = re.findall(r"/dev/(?:pts/\d+|ttys\d+)", text)
        if hits:
            pty = hits[-1]
            break
        time.sleep(0.5)
    if not pty:
        proc.send_signal(signal.SIGTERM)
        raise RuntimeError(f"QEMU PTY not announced; see {qlog}")
    print(f"QEMU PTY: {pty}", flush=True)
    (ART / "pty.txt").write_text(pty + "\n", encoding="utf-8")
    return proc, pty


def wait_bridge(pty: str, proc: subprocess.Popen, *, seconds: float) -> None:
    last = ""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU died while waiting for m4adb (rc={proc.returncode})")
        try:
            last = m4adb(pty, ["ping"], timeout=30, check=False)
            if '"protocol"' in last and '"firmware"' in last:
                (ART / "ping.json").write_text(last, encoding="utf-8")
                print("m4adb bridge ready", flush=True)
                return
        except (subprocess.TimeoutExpired, RuntimeError):
            pass
        time.sleep(1)
    (ART / "ping-fail.txt").write_text(last, encoding="utf-8")
    raise RuntimeError("m4adb bridge never became ready")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", help="path to patched qemu-system-xtensa")
    p.add_argument("--skip-build", action="store_true", help="reuse last plugin-debug firmware build")
    p.add_argument("--plugin-src", help="path to plugin source tree for m4adb install")
    p.add_argument("--app-id", help="plugin app id for launch (e.g. com.fanqie.client)")
    p.add_argument("--seconds", type=float, default=90.0, help="max wait for m4adb ready")
    p.add_argument("--keep-alive", action="store_true",
                   help="leave QEMU running after setup (Ctrl-C to stop)")
    p.add_argument("--no-net-check", action="store_true", help="skip wifi_status assertion")
    args = p.parse_args(argv)

    try:
        if TMP.exists() and not args.skip_build:
            # keep artifacts folder; rebuild flash
            pass
        ART.mkdir(parents=True, exist_ok=True)

        qemu = find_qemu(args.qemu)
        if args.skip_build:
            bdir = ROOT / "firmware/.pio/build/murphy_m4_qemu_plugin"
            if not (bdir / "firmware.bin").is_file():
                raise SystemExit("--skip-build but no prior murphy_m4_qemu_plugin build")
        else:
            # Ensure open-m4-sdk deps present
            if not (ROOT / "firmware/open-m4-sdk/libs/hardware/BoardConfig").exists():
                run(["bash", "scripts/bootstrap_deps.sh"], cwd=ROOT)
            bdir = build_firmware()

        flash = compose_flash(bdir)
        sd = make_sd()
        proc, pty = boot(qemu, flash, sd, serial_log=ART / "guest.serial.log")
        try:
            wait_bridge(pty, proc, seconds=args.seconds)

            if not args.no_net_check:
                st = m4adb(pty, ["wifi_status"], timeout=40, check=False)
                (ART / "wifi_status.json").write_text(st, encoding="utf-8")
                if "qemu-openeth" not in st and '"ready":true' not in st and '"connected":true' not in st:
                    print("WARNING: wifi_status does not show open_eth ready yet:\n", st, flush=True)
                else:
                    print("network ready (open_eth / WiFi-compat)", flush=True)

            if args.plugin_src:
                src = Path(args.plugin_src).expanduser().resolve()
                if not src.is_dir():
                    raise SystemExit(f"plugin src not found: {src}")
                install_out = m4adb(
                    pty,
                    ["install", str(src), "--transport", "usb",
                     "--ready-timeout", "30", "--commit-timeout", "180",
                     "--overall-timeout", "600"],
                    timeout=660, check=False,
                )
                (ART / "install.log").write_text(install_out, encoding="utf-8")
                ok = ("安装完成" in install_out or "installed" in install_out.lower()
                      or '"id"' in install_out)
                print("install_ok=", ok, flush=True)
                if ok and args.app_id:
                    launch_out = m4adb(pty, ["launch", args.app_id], timeout=90, check=False)
                    (ART / "launch.log").write_text(launch_out, encoding="utf-8")
                    print("launch_out:\n", launch_out, flush=True)

            print(f"\n=== Plugin debug session ===\n"
                  f"PTY: {pty}\n"
                  f"Artifacts: {ART}\n"
                  f"m4adb example:\n"
                  f"  python3 firmware/scripts/m4adb.py --port {pty} --no-daemon ping\n"
                  f"  python3 firmware/scripts/m4adb.py --port {pty} --no-daemon wifi_status\n"
                  f"  python3 firmware/scripts/m4adb.py --port {pty} --no-daemon install <plugin> --transport usb\n",
                  flush=True)

            if args.keep_alive:
                print("keep-alive: Ctrl-C to stop QEMU", flush=True)
                while proc.poll() is None:
                    time.sleep(1)
            return 0
        finally:
            if proc.poll() is None and not args.keep_alive:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"FAIL {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
