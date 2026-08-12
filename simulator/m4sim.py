#!/usr/bin/env python3
"""Unified Murphy M4 firmware-bin simulator entry (clean-machine path).

Source of truth: the local chain that already boots firmware under patched QEMU
(see simulator/qemu/LOCAL_RUNTIME_CHAIN.md). This CLI does not invent a second
stack for CI — it wraps the same builders, flash composer, machine flags, and
protocol-level m4adb readiness used on a working developer machine.

Examples::

  ./m4sim info
  ./m4sim build-qemu -j 6
  ./m4sim run path/to/firmware.bin          # app bin + companion BL/partitions
  ./m4sim run path/to/16m-flash.bin         # full flash image
  ./m4sim run --plugin-debug                # rebuild murphy_m4_qemu_plugin path
  ./m4sim screenshot out.png
  ./m4sim key back
  ./m4sim ui
  ./m4sim stop
  ./m4sim test smoke
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import shutil
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
QEMU_DIR = ROOT / "simulator" / "qemu"
TOOLS = ROOT / "simulator" / "tools"
M4ADB = ROOT / "firmware" / "scripts" / "m4adb.py"
FLASH_SIZE = 16 * 1024 * 1024

DEFAULT_SESSION = Path(os.environ.get("M4SIM_TMP", "/tmp/m4sim"))
ART = DEFAULT_SESSION / "artifacts"
STATE = DEFAULT_SESSION / "state.json"

# Automatic discovery is intentionally v3-only. Older patched QEMU builds may
# still be used explicitly with --qemu for archaeology, but must never become
# the implicit runtime source of truth.
QEMU_CANDIDATES = [
    Path.home() / ".cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa",
    Path.home() / ".cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa-unsigned",
]


class M4SimError(RuntimeError):
    pass


def _run(cmd: list[str], *, cwd: Path | None = None, check: bool = True,
         env: dict[str, str] | None = None, timeout: float | None = None) -> subprocess.CompletedProcess:
    print("+", " ".join(shlex.quote(str(x)) for x in cmd), flush=True)
    return subprocess.run(
        cmd, cwd=cwd or ROOT, check=check, env=env, timeout=timeout,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )


def _print_cp(cp: subprocess.CompletedProcess) -> None:
    if cp.stdout:
        print(cp.stdout, end="" if cp.stdout.endswith("\n") else "\n", flush=True)


def load_state() -> dict[str, Any]:
    if not STATE.is_file():
        return {}
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def save_state(data: dict[str, Any]) -> None:
    DEFAULT_SESSION.mkdir(parents=True, exist_ok=True)
    ART.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def find_qemu(explicit: str | None = None, *, build_if_missing: bool = False) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    env = os.environ.get("QEMU_XTENSA")
    if env:
        candidates.append(Path(env))
    candidates.extend(QEMU_CANDIDATES)
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c.resolve()
    if build_if_missing:
        print("patched QEMU not found; building via simulator/qemu/build.py …", flush=True)
        cp = _run([sys.executable, str(QEMU_DIR / "build.py"), "-j", str(os.cpu_count() or 4)], check=False)
        _print_cp(cp)
        if cp.returncode != 0:
            raise M4SimError("QEMU build failed; see output above")
        for c in QEMU_CANDIDATES:
            if c.is_file():
                return c.resolve()
    raise M4SimError(
        "patched qemu-system-xtensa not found.\n"
        "  python3 simulator/qemu/build.py -j 6\n"
        "  export QEMU_XTENSA=$HOME/.cache/murphy-m4/espressif-qemu-v3/"
        "build-murphy-v3/qemu-system-xtensa"
    )


def check_host_deps() -> list[str]:
    missing: list[str] = []
    for name in ("git", "ninja", "python3"):
        if shutil.which(name) is None:
            missing.append(name)
    try:
        import serial  # noqa: F401
    except ImportError:
        missing.append("python:pyserial (pip install -r simulator/qemu/requirements.txt)")
    return missing


def cmd_info(_: argparse.Namespace) -> int:
    missing = check_host_deps()
    qemu = None
    try:
        qemu = find_qemu(None, build_if_missing=False)
    except M4SimError as exc:
        qemu_err = str(exc)
    else:
        qemu_err = ""
    st = load_state()
    print("m4sim — Murphy M4 BIN simulator")
    print(f"  repo:   {ROOT}")
    print(f"  session:{DEFAULT_SESSION}")
    print(f"  qemu:   {qemu if qemu else '(missing)'}")
    if qemu_err and not qemu:
        print(f"          {qemu_err.splitlines()[0]}")
    if qemu and qemu.is_file():
        try:
            ver = subprocess.check_output([str(qemu), "--version"], text=True, timeout=5)
            print(f"  version:{ver.splitlines()[0]}")
            machines = subprocess.check_output(
                [str(qemu), "-machine", "help"], text=True, timeout=10, stderr=subprocess.STDOUT,
            )
            print(f"  murphy-m4 machine: {'yes' if 'murphy-m4' in machines else 'NO'}")
        except (subprocess.SubprocessError, OSError) as exc:
            print(f"  qemu probe failed: {exc}")
    print(f"  upstream: {QEMU_DIR / 'patches' / 'upstream.json'}")
    print(f"  series:   {QEMU_DIR / 'patches' / 'series-v3'}")
    print(f"  state:    pid={st.get('pid')} pty={st.get('pty')} running={_session_alive(st)}")
    if missing:
        print("  missing deps:")
        for m in missing:
            print(f"    - {m}")
        return 2
    return 0


def cmd_build_qemu(args: argparse.Namespace) -> int:
    jobs = str(args.jobs or os.cpu_count() or 4)
    cmd = [sys.executable, str(QEMU_DIR / "build.py"), "-j", jobs]
    if args.reconfigure:
        cmd.append("--reconfigure")
    if args.force_source:
        cmd.append("--force-source")
    cp = _run(cmd, check=False)
    _print_cp(cp)
    return cp.returncode


def _pio_env() -> dict[str, str]:
    env = os.environ.copy()
    penv = Path.home() / ".platformio" / "penv" / "bin"
    if penv.is_dir():
        env["PATH"] = str(penv) + os.pathsep + env.get("PATH", "")
    return env


def compose_flash_from_build(build_dir: Path, out: Path) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    cp = _run([
        sys.executable, str(TOOLS / "murphy_flash_image.py"),
        "--build-dir", str(build_dir), "-o", str(out),
    ], check=False)
    _print_cp(cp)
    if cp.returncode != 0 or not out.is_file():
        raise M4SimError(f"flash compose failed → {out}")
    if out.stat().st_size != FLASH_SIZE:
        raise M4SimError(f"flash size {out.stat().st_size} != {FLASH_SIZE}")
    return out


def compose_flash_from_app_bin(app_bin: Path, build_dir: Path | None, out: Path) -> Path:
    """Wrap a lone firmware.bin with bootloader+partitions from a PIO build dir."""
    if build_dir is None:
        for cand in (
            ROOT / "firmware/.pio/build/murphy_m4_qemu_plugin",
            ROOT / "firmware/.pio/build/murphy_m4_qemu",
            ROOT / "firmware/.pio/build/murphy_m4",
        ):
            if (cand / "bootloader.bin").is_file() and (cand / "partitions.bin").is_file():
                build_dir = cand
                break
    if build_dir is None or not build_dir.is_dir():
        raise M4SimError(
            "firmware.bin needs companion bootloader/partitions.\n"
            "  Pass --build-dir firmware/.pio/build/<env>\n"
            "  or build first: pio run -e murphy_m4_qemu_plugin"
        )
    out.parent.mkdir(parents=True, exist_ok=True)
    cp = _run([
        sys.executable, str(TOOLS / "murphy_flash_image.py"),
        "--bootloader", str(build_dir / "bootloader.bin"),
        "--partitions", str(build_dir / "partitions.bin"),
        "--firmware", str(app_bin),
        "-o", str(out),
    ], check=False)
    _print_cp(cp)
    if cp.returncode != 0 or not out.is_file():
        raise M4SimError("flash compose from app bin failed")
    return out


def ensure_sd(*, fresh: bool, size_mb: int) -> Path:
    ART.mkdir(parents=True, exist_ok=True)
    sd = ART / "murphy-sd.img"
    if sd.is_file() and sd.stat().st_size >= size_mb * 1024 * 1024 and not fresh:
        return sd
    cp = _run([
        sys.executable, str(QEMU_DIR / "make_sd_image.py"),
        str(sd), "--size-mb", str(size_mb), "--force",
    ], check=False)
    _print_cp(cp)
    if cp.returncode != 0:
        raise M4SimError("SD image create failed")
    return sd


def m4adb_once(pty: str, args: list[str], *, timeout: float = 12.0) -> tuple[int, str]:
    """One-shot m4adb with wall-clock kill. Protocol readiness uses this."""
    inner = max(2.0, timeout - 1.5)
    ready = min(inner, 8.0)
    penv_py = Path.home() / ".platformio/penv/bin/python"
    py = str(penv_py if penv_py.is_file() else sys.executable)
    cmd = [
        py, str(M4ADB),
        "--port", pty, "--no-daemon",
        "--timeout", str(inner),
        "--ready-timeout", str(ready),
        *args,
    ]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    try:
        cp = subprocess.run(
            cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout,
        )
        out = cp.stdout or ""
        print(out, end="" if out.endswith("\n") or not out else "\n", flush=True)
        return cp.returncode, out
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        msg = f"HARD_TIMEOUT {timeout:.0f}s m4adb {' '.join(args)}\n{out}"
        print(msg, flush=True)
        return 124, msg


def wait_m4adb_ready(pty: str, proc: subprocess.Popen, *, seconds: float,
                     qemu_log: Path) -> dict[str, Any]:
    """Protocol-level readiness: short reconnects until ping JSON is valid."""
    deadline = time.time() + seconds
    last = ""
    attempt = 0
    while time.time() < deadline:
        if proc.poll() is not None:
            qlog = qemu_log.read_text(encoding="utf-8", errors="replace") if qemu_log.is_file() else ""
            raise M4SimError(
                f"QEMU exited while waiting for m4adb (rc={proc.returncode})\n"
                f"--- qemu log tail ---\n{qlog[-3000:]}"
            )
        attempt += 1
        # Short connect budget; no fixed multi-minute sleep.
        rc, last = m4adb_once(pty, ["ping"], timeout=8.0)
        if rc == 0 and '"protocol"' in last and '"firmware"' in last:
            # Prefer structured parse when possible.
            blob = _json_blob(last)
            if blob and "protocol" in blob and "firmware" in blob:
                (ART / "ping.json").write_text(json.dumps(blob, indent=2) + "\n", encoding="utf-8")
                print(f"m4adb READY after {attempt} attempt(s)", flush=True)
                return blob
            if '"protocol"' in last:
                print(f"m4adb READY (text) after {attempt} attempt(s)", flush=True)
                return {"raw": last}
        time.sleep(0.4)
    (ART / "ping-fail.txt").write_text(last, encoding="utf-8")
    qlog = qemu_log.read_text(encoding="utf-8", errors="replace") if qemu_log.is_file() else ""
    raise M4SimError(
        f"m4adb never became ready within {seconds:.0f}s (attempts={attempt})\n"
        f"last ping output:\n{last[-1500:]}\n"
        f"--- qemu log tail ---\n{qlog[-3000:]}"
    )


def _json_blob(text: str) -> dict[str, Any] | None:
    start = text.find("{")
    if start < 0:
        return None
    # try from each brace
    for i, ch in enumerate(text):
        if ch != "{":
            continue
        try:
            return json.loads(text[i:])
        except json.JSONDecodeError:
            continue
    return None


def boot_qemu(qemu: Path, flash: Path, sd: Path | None, *,
              open_eth: bool = True, psram_mb: int = 8) -> tuple[subprocess.Popen, str, Path]:
    ART.mkdir(parents=True, exist_ok=True)
    qlog = ART / "qemu.log"
    frame = ART / "ssd1677-frame.pbm"
    frame.unlink(missing_ok=True)
    (ART / "frame-file.txt").write_text(str(frame) + "\n", encoding="utf-8")
    cmd = [
        str(qemu), "-nographic", "-monitor", "none",
        "-machine", "murphy-m4", "-m", f"{psram_mb}M",
        "-drive", f"file={flash},if=mtd,format=raw",
    ]
    if sd is not None:
        cmd += ["-drive", f"file={sd},if=sd,format=raw"]
    cmd += [
        "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
        "-global", "driver=esp32s3.gpio,property=input-default,value=0x100000000007",
        "-global", "driver=ssi_psram,property=is_octal,value=true",
        "-global", f"driver=murphy-ssd1677,property=frame-file,value={frame}",
        "-global", "driver=murphy-ssd1677,property=busy-ms,value=20",
        "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
        "-serial", "pty",
    ]
    if open_eth:
        cmd += ["-nic", "user,model=open_eth,hostfwd=tcp::18080-:80,hostfwd=tcp::18081-:81"]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    with qlog.open("w", encoding="utf-8") as lf:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT, text=True)

    pty = ""
    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            text = qlog.read_text(encoding="utf-8", errors="replace")
            raise M4SimError(f"QEMU exited early rc={proc.returncode}\n{text[-2000:]}")
        text = qlog.read_text(encoding="utf-8", errors="replace")
        hits = re.findall(r"/dev/(?:pts/\d+|ttys\d+)", text)
        if hits:
            pty = hits[-1]
            break
        time.sleep(0.25)
    if not pty:
        proc.send_signal(signal.SIGTERM)
        raise M4SimError(f"QEMU PTY not announced; see {qlog}")
    print(f"QEMU PTY: {pty}", flush=True)
    (ART / "pty.txt").write_text(pty + "\n", encoding="utf-8")
    return proc, pty, qlog


def _session_alive(st: dict[str, Any] | None = None) -> bool:
    st = st if st is not None else load_state()
    pid = st.get("pid")
    if not pid:
        return False
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError):
        return False


def stop_session() -> None:
    st = load_state()
    pid = st.get("pid")
    if pid:
        try:
            os.kill(int(pid), signal.SIGTERM)
            time.sleep(0.5)
            os.kill(int(pid), 0)
            os.kill(int(pid), signal.SIGKILL)
        except OSError:
            pass
    if STATE.is_file():
        STATE.unlink()
    print("session stopped", flush=True)


def cmd_stop(_: argparse.Namespace) -> int:
    stop_session()
    return 0


def resolve_flash(args: argparse.Namespace) -> Path:
    ART.mkdir(parents=True, exist_ok=True)
    out = ART / "flash-16m.bin"

    if args.plugin_debug:
        env = _pio_env()
        if not (ROOT / "firmware/open-m4-sdk/libs/hardware/BoardConfig").exists():
            cp = _run(["bash", "scripts/bootstrap_deps.sh"], check=False)
            _print_cp(cp)
        if not args.skip_build:
            cp = _run(["pio", "run", "-e", "murphy_m4_qemu_plugin"], cwd=ROOT / "firmware",
                      env=env, check=False)
            _print_cp(cp)
            if cp.returncode != 0:
                raise M4SimError("pio run -e murphy_m4_qemu_plugin failed")
        bdir = ROOT / "firmware/.pio/build/murphy_m4_qemu_plugin"
        return compose_flash_from_build(bdir, out)

    image = Path(args.image).expanduser().resolve() if args.image else None
    if image is None:
        raise M4SimError("pass a firmware.bin / 16MiB flash path, or --plugin-debug")

    if not image.is_file():
        raise M4SimError(f"not a file: {image}")

    if image.stat().st_size == FLASH_SIZE:
        shutil.copy2(image, out)
        return out

    build_dir = Path(args.build_dir).expanduser().resolve() if args.build_dir else None
    return compose_flash_from_app_bin(image, build_dir, out)


def cmd_run(args: argparse.Namespace) -> int:
    missing = check_host_deps()
    hard = [m for m in missing if not m.startswith("python:")]
    if hard and not args.qemu:
        pass

    if _session_alive():
        if args.force:
            stop_session()
        else:
            raise M4SimError("session already running; ./m4sim stop  (or --force)")

    qemu = find_qemu(args.qemu, build_if_missing=bool(args.build_qemu))
    flash = resolve_flash(args)
    sd = None if args.no_sd else ensure_sd(fresh=bool(args.fresh_sd), size_mb=int(args.sd_size_mb))

    proc, pty, qlog = boot_qemu(qemu, flash, sd, open_eth=not args.no_net, psram_mb=int(args.psram_mb))
    try:
        info = wait_m4adb_ready(pty, proc, seconds=float(args.ready_seconds), qemu_log=qlog)
        save_state({
            "pid": proc.pid,
            "pty": pty,
            "qemu": str(qemu),
            "flash": str(flash),
            "sd": str(sd) if sd else None,
            "frame": str(ART / "ssd1677-frame.pbm"),
            "qemu_log": str(qlog),
            "started": time.time(),
            "ping": info if isinstance(info, dict) else {"raw": str(info)},
        })
        print(
            f"\n=== m4sim session ===\n"
            f"PTY:   {pty}\n"
            f"PID:   {proc.pid}\n"
            f"ART:   {ART}\n"
            f"frame: {ART / 'ssd1677-frame.pbm'}\n"
            f"m4adb: python3 firmware/scripts/m4adb.py --port {pty} --no-daemon ping\n"
            f"stop:  ./m4sim stop\n",
            flush=True,
        )
        if args.keep_alive:
            print("keep-alive: Ctrl-C to stop", flush=True)
            try:
                while proc.poll() is None:
                    time.sleep(1)
            except KeyboardInterrupt:
                pass
            stop_session()
            return 0
        if args.wait:
            try:
                proc.wait()
            except KeyboardInterrupt:
                stop_session()
            return proc.returncode or 0
        # Default and explicit --detach both return after protocol readiness.
        return 0
    except Exception:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        raise


def require_session() -> dict[str, Any]:
    st = load_state()
    if not _session_alive(st):
        raise M4SimError("no running session; ./m4sim run ... first")
    if not st.get("pty"):
        raise M4SimError("session missing pty")
    return st


def cmd_screenshot(args: argparse.Namespace) -> int:
    st = require_session()
    out = Path(args.output).expanduser().resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    frame = Path(st.get("frame") or (ART / "ssd1677-frame.pbm"))
    if frame.is_file() and frame.stat().st_size > 32:
        if out.suffix.lower() == ".pbm":
            shutil.copy2(frame, out)
            print(f"wrote {out} from {frame}")
            return 0
        try:
            from PIL import Image
            img = Image.open(frame)
            img.save(out)
            print(f"wrote {out}")
            return 0
        except Exception:
            pbm_out = out.with_suffix(".pbm")
            shutil.copy2(frame, pbm_out)
            print(f"Pillow missing/failed; wrote {pbm_out}")
            return 0
    rc, text = m4adb_once(st["pty"], ["screenshot", str(out)], timeout=20)
    if rc != 0:
        raise M4SimError(f"screenshot failed\n{text}")
    return 0


def cmd_key(args: argparse.Namespace) -> int:
    st = require_session()
    name = args.name.lower()
    aliases = {
        "back": ["key", "back"],
        "enter": ["key", "confirm"],
        "confirm": ["key", "confirm"],
        "left": ["key", "left"],
        "right": ["key", "right"],
        "up": ["key", "up"],
        "down": ["key", "down"],
    }
    m4args = aliases.get(name, ["key", name])
    rc, text = m4adb_once(st["pty"], m4args, timeout=12)
    if rc != 0:
        if name in ("enter", "confirm"):
            rc2, text2 = m4adb_once(st["pty"], ["tap", "240", "400"], timeout=12)
            if rc2 == 0:
                return 0
            raise M4SimError(f"key failed\n{text}\n{text2}")
        raise M4SimError(f"key failed\n{text}")
    return 0


def cmd_ui(args: argparse.Namespace) -> int:
    st = require_session()
    if args.json:
        rc, text = m4adb_once(st["pty"], ["status"], timeout=12)
        if rc != 0:
            raise M4SimError(text)
        return 0
    viewer = TOOLS / "m4_screen_viewer.py"
    frame = st.get("frame") or str(ART / "ssd1677-frame.pbm")
    cmd = [sys.executable, str(viewer), "--pty", st["pty"], "--frame-file", frame]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    return subprocess.call(cmd, cwd=ROOT)


def cmd_test_smoke(args: argparse.Namespace) -> int:
    """Clean-session smoke: build qemu if needed, boot, ping, optional key, stop."""
    global DEFAULT_SESSION, ART, STATE
    smoke_root = Path(os.environ.get("M4SIM_SMOKE_TMP", "/tmp/m4sim-smoke"))
    if smoke_root.exists():
        shutil.rmtree(smoke_root, ignore_errors=True)
    DEFAULT_SESSION = smoke_root
    ART = smoke_root / "artifacts"
    STATE = smoke_root / "state.json"
    os.environ["M4SIM_TMP"] = str(smoke_root)

    ns = argparse.Namespace(
        image=args.image,
        plugin_debug=bool(args.plugin_debug or not args.image),
        build_dir=args.build_dir,
        skip_build=bool(args.skip_build),
        qemu=args.qemu,
        build_qemu=True,
        fresh_sd=True,
        sd_size_mb=64,
        no_sd=False,
        no_net=False,
        psram_mb=8,
        ready_seconds=args.ready_seconds,
        detach=True,
        keep_alive=False,
        wait=False,
        force=True,
    )
    try:
        cmd_run(ns)
        st = require_session()
        rc, out = m4adb_once(st["pty"], ["ping"], timeout=10)
        if rc != 0 or '"protocol"' not in out:
            raise M4SimError(f"smoke ping failed\n{out}")
        m4adb_once(st["pty"], ["status"], timeout=10)
        frame = Path(st.get("frame") or (ART / "ssd1677-frame.pbm"))
        for _ in range(20):
            if frame.is_file() and frame.stat().st_size > 32:
                break
            time.sleep(0.5)
        shot = ART / "smoke.pbm"
        if frame.is_file() and frame.stat().st_size > 32:
            shutil.copy2(frame, shot)
            print(f"smoke frame: {shot} ({shot.stat().st_size} bytes)")
        else:
            print("smoke: no ssd1677 frame yet (boot may still be early)")
        print("SMOKE PASS")
        return 0
    finally:
        stop_session()


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="m4sim", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="show qemu/session/deps")

    bq = sub.add_parser("build-qemu", help="build patched qemu-system-xtensa (v3 series)")
    bq.add_argument("-j", "--jobs", type=int, default=0)
    bq.add_argument("--reconfigure", action="store_true")
    bq.add_argument("--force-source", action="store_true")

    run = sub.add_parser("run", help="compose flash, boot murphy-m4, wait m4adb ready")
    run.add_argument("image", nargs="?", help="firmware.bin or 16MiB flash image")
    run.add_argument("--plugin-debug", action="store_true",
                     help="build/run murphy_m4_qemu_plugin path (interactive m4adb)")
    run.add_argument("--build-dir", help="PIO build dir for bootloader/partitions")
    run.add_argument("--skip-build", action="store_true")
    run.add_argument("--qemu", help="path to patched qemu-system-xtensa")
    run.add_argument("--build-qemu", action="store_true", help="build QEMU if missing")
    run.add_argument("--fresh-sd", action="store_true")
    run.add_argument("--sd-size-mb", type=int, default=64)
    run.add_argument("--no-sd", action="store_true")
    run.add_argument("--no-net", action="store_true")
    run.add_argument("--psram-mb", type=int, default=8, choices=(8, 16, 32))
    run.add_argument("--ready-seconds", type=float, default=90.0)
    mode = run.add_mutually_exclusive_group()
    mode.add_argument("--detach", action="store_true", help="return after ready (default)")
    mode.add_argument("--keep-alive", action="store_true", help="block until Ctrl-C")
    mode.add_argument("--wait", action="store_true", help="block until QEMU exits")
    run.add_argument("--force", action="store_true", help="stop existing session first")

    sub.add_parser("stop", help="stop QEMU session")

    sc = sub.add_parser("screenshot", help="save current frame")
    sc.add_argument("output", nargs="?", default="m4sim-screen.png")

    ky = sub.add_parser("key", help="send a key (back/enter/left/right/…)")
    ky.add_argument("name")

    ui = sub.add_parser("ui", help="open screen viewer or dump status")
    ui.add_argument("--json", action="store_true", help="print m4adb status JSON only")

    sm = sub.add_parser("test", help="built-in tests")
    sm_sub = sm.add_subparsers(dest="test_cmd", required=True)
    smoke = sm_sub.add_parser("smoke", help="clean boot + protocol ping")
    smoke.add_argument("image", nargs="?", help="optional firmware/flash path")
    smoke.add_argument("--plugin-debug", action="store_true", default=True)
    smoke.add_argument("--build-dir")
    smoke.add_argument("--skip-build", action="store_true")
    smoke.add_argument("--qemu")
    smoke.add_argument("--ready-seconds", type=float, default=120.0)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.cmd == "info":
            return cmd_info(args)
        if args.cmd == "build-qemu":
            return cmd_build_qemu(args)
        if args.cmd == "run":
            return cmd_run(args)
        if args.cmd == "stop":
            return cmd_stop(args)
        if args.cmd == "screenshot":
            return cmd_screenshot(args)
        if args.cmd == "key":
            return cmd_key(args)
        if args.cmd == "ui":
            return cmd_ui(args)
        if args.cmd == "test":
            if args.test_cmd == "smoke":
                return cmd_test_smoke(args)
        parser.error(f"unknown command {args.cmd}")
    except M4SimError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as exc:
        print(f"error: command failed ({exc.returncode})", file=sys.stderr)
        return exc.returncode or 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
