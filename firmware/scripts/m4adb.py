#!/usr/bin/env python3
"""m4adb — Murphy M4 USB serial automated debug CLI.

Requires on-device Developer Options → USB serial control enabled.
Dependency: Python 3 stdlib + optional pyserial for real devices.
Does not flash hardware, touch NVS, or export credentials.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from m4adb_lib import package as pkg  # noqa: E402
from m4adb_lib.client import BridgeError, Client  # noqa: E402
from m4adb_lib.daemon import BridgeDaemon, DaemonTransport, daemon_alive, socket_path_for_port, stop_daemon  # noqa: E402
from m4adb_lib.journey import load_journey, run_journey  # noqa: E402
from m4adb_lib.mock_device import MockDevice  # noqa: E402
from m4adb_lib.transport import MockTransport, SerialTransport, auto_port, list_serial_devices  # noqa: E402
from m4adb_lib.watch import watch_loop  # noqa: E402

DEFAULT_CACHE = ROOT / "build" / "m4adb" / "cache"
DEFAULT_ARTIFACTS = ROOT / "build" / "m4adb" / "runs"


def _resolve_ready_timeout(args: argparse.Namespace, ready_timeout: float | None = None) -> float:
    """Host wait for bridge handshake.

    Defaults stay generous (install/boot after USB reset). Explicit values are
    honored without the old hard floor of 15s so smoke tests can fail fast::

      m4adb --timeout 5 --ready-timeout 5 --no-daemon ping
    """
    if ready_timeout is not None:
        return max(0.5, float(ready_timeout))
    explicit = getattr(args, "ready_timeout", None)
    if explicit is not None:
        return max(0.5, float(explicit))
    base = float(getattr(args, "timeout", 10))
    # Implicit path only: keep a 15s floor for real-device reconnect/reset.
    return max(15.0, base)


def _open_direct_client(args: argparse.Namespace, ready_timeout: float | None = None) -> Client:
    if getattr(args, "mock", False):
        dev = MockDevice()
        # Preload weread fixture as installed for launch tests if present
        return Client(MockTransport(dev), default_timeout=getattr(args, "timeout", 10))
    port = args.port or auto_port()
    if not port:
        raise SystemExit("未找到串口设备。请用 --port 指定，或先运行 devices / doctor。")
    baud = int(getattr(args, "baud", 115200))
    client = Client(SerialTransport(port, baud), default_timeout=float(getattr(args, "timeout", 10)))
    # ESP32-S3 USB Serial/JTAG may reset when macOS establishes the CDC
    # session. Keep this same handle open and retry the idempotent handshake
    # until the firmware bridge is ready.
    # install/sync need a longer ready window: open may reset the chip and
    # Home/bridge can take 20–45s after e-ink refresh.
    ready = _resolve_ready_timeout(args, ready_timeout)
    print(f"[m4adb] 串口 {port}，等待就绪（最多 {ready:.0f}s）…", flush=True)
    t0 = time.time()
    client.wait_ready(timeout=ready)
    print(f"[m4adb] 已就绪 ({time.time() - t0:.1f}s)", flush=True)
    return client


def _open_client(args: argparse.Namespace, ready_timeout: float | None = None) -> Client:
    """Open a client, reusing one resident USB owner whenever possible."""
    if getattr(args, "mock", False) or getattr(args, "no_daemon", False):
        return _open_direct_client(args, ready_timeout)
    port = args.port or auto_port()
    if not port:
        raise SystemExit("未找到串口设备。请用 --port 指定，或先运行 devices / doctor。")
    path = socket_path_for_port(port)
    base = float(getattr(args, "timeout", 10))
    ready = _resolve_ready_timeout(args, ready_timeout)

    def connect_existing() -> Client | None:
        c: Client | None = None
        try:
            c = Client(DaemonTransport(path, timeout=1.5), default_timeout=base)
            c.wait_ready(timeout=min(8.0, ready))
            print(f"[m4adb] 复用常驻 USB 调试会话 {path}", flush=True)
            return c
        except (OSError, RuntimeError, BridgeError):
            if c is not None:
                c.close()
            return None

    existing = connect_existing()
    if existing is not None:
        return existing

    # No responsive daemon: a stale process may still own the socket with a
    # dead serial handle (device reset/reboot). Ask it to exit first so the
    # new owner below can bind the socket — never spawn two owners.
    stop_daemon(path)
    # Give the old daemon a moment to unlink its socket.
    time.sleep(0.4)
    for _ in range(5):
        if not daemon_alive(path):
            break
        time.sleep(0.2)

    # Spawn exactly one owner, then connect over its socket.  The child owns
    # the hardware handle; this process never opens USB directly.
    log_path = ROOT / "build" / "m4adb" / "daemon.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = log_path.open("a", encoding="utf-8")
    cmd = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--port",
        port,
        "--baud",
        str(int(getattr(args, "baud", 115200))),
        "daemon",
        "--socket",
        str(path),
        "--ready-timeout",
        str(ready),
    ]
    try:
        subprocess.Popen(cmd, stdout=log, stderr=log, start_new_session=True, close_fds=True)
    finally:
        log.close()
    deadline = time.time() + ready + 5.0
    while time.time() < deadline:
        c = connect_existing()
        if c is not None:
            return c
        time.sleep(0.2)
    # If daemon startup failed, do not leave a stale owner behind.  A direct
    # fallback is allowed only after the child is gone, preventing two USB
    # handles from racing and resetting the board.
    stop_daemon(path)
    raise BridgeError("daemon_start_failed", f"常驻 USB 调试服务启动失败；详见 {log_path}")


def cmd_daemon(args: argparse.Namespace) -> int:
    port = args.port or auto_port()
    if not port:
        print("未找到串口设备", file=sys.stderr)
        return 1
    path = Path(args.socket) if args.socket else socket_path_for_port(port)
    return BridgeDaemon(port, int(args.baud), path).serve(ready_timeout=float(args.ready_timeout))


def cmd_daemon_stop(args: argparse.Namespace) -> int:
    port = args.port or auto_port()
    if not port:
        print("未找到串口设备", file=sys.stderr)
        return 1
    path = socket_path_for_port(port)
    if stop_daemon(path):
        print(f"已请求停止常驻 USB 调试会话 {path}")
        return 0
    print("没有运行中的常驻 USB 调试会话")
    return 1


def _install_timeouts(args: argparse.Namespace) -> tuple[float, float, float]:
    """Return (commit_timeout, overall_timeout, ready_timeout) for install/sync."""
    commit = float(getattr(args, "commit_timeout", 0) or 0) or 180.0
    overall = float(getattr(args, "overall_timeout", 0) or 0) or max(240.0, commit + 90.0)
    ready = float(getattr(args, "ready_timeout", 0) or 0) or max(60.0, float(getattr(args, "timeout", 10)))
    return commit, overall, ready


def _install_progress(done: int, total: int, phase: str) -> None:
    """Print live install progress (host + device prg frames)."""
    if phase == "begin":
        print("[m4adb] USB install_begin…", flush=True)
    elif phase == "upload" and total > 0:
        pct = 100.0 * done / total
        print(f"[m4adb] USB 上传 {done}/{total} ({pct:.0f}%)", flush=True)
    elif phase == "commit":
        print("[m4adb] USB install_commit（设备解压）…", flush=True)
    elif phase == "wifi_serve":
        print("[m4adb] [  0%] Wi-Fi：启动本机临时 HTTP…", flush=True)
    elif phase.startswith("wifi_selfcheck"):
        print(f"[m4adb] [  2%] Wi-Fi：本机自检 {phase.split(':', 1)[-1] if ':' in phase else ''}", flush=True)
    elif phase.startswith("wifi_url"):
        url = phase.split(":", 1)[-1] if ":" in phase else ""
        print(f"[m4adb] [  5%] Wi-Fi：等待设备拉取 {url}", flush=True)
    elif phase == "wifi_send":
        print("[m4adb] [  8%] Wi-Fi：已发送 install_http，等待设备进度…", flush=True)
    elif phase.startswith("wifi_device"):
        print(f"[m4adb] [  6%] Wi-Fi：设备地址 {phase.split(':', 1)[-1] if ':' in phase else ''}", flush=True)
    elif phase.startswith("device:"):
        # device:download:1234/32124 or device:wifi
        rest = phase[len("device:") :]
        parts = rest.split(":")
        dphase = parts[0] if parts else rest
        detail = parts[1] if len(parts) > 1 else ""
        labels = {
            "accepted": "设备已接收",
            "wifi": "设备连接 Wi-Fi",
            "wifi_ok": "设备 Wi-Fi 已就绪",
            "http_begin": "设备开始 HTTP",
            "http_get": "设备 HTTP GET",
            "download": "设备下载中",
            "verify": "设备校验 SHA",
            "install": "设备安装中",
        }
        label = labels.get(dphase, dphase)
        bar = f"{done:3d}%" if total == 100 and done >= 0 else "  …"
        extra = f" {detail}" if detail else ""
        print(f"[m4adb] [{bar}] {label}{extra}", flush=True)
    elif phase.startswith("host_wait:"):
        print(f"[m4adb] [  …] 等待设备回报（{phase[len('host_wait:'):]}）", flush=True)
    elif phase == "wifi_done":
        print("[m4adb] [100%] Wi-Fi：完成", flush=True)
    elif phase.startswith("wifi_fallback:"):
        print(f"[m4adb] Wi-Fi 不可用（{phase.split(':', 1)[1]}），回退 USB…", flush=True)


def cmd_devices(_args: argparse.Namespace) -> int:
    devs = list_serial_devices()
    if not devs:
        print("未发现候选串口设备")
        return 1
    for d in devs:
        print(f"{d['device']}\t{d.get('description', '')}\t{d.get('hwid', '')}")
    return 0


def cmd_ping(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.ping(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_status(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.status(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_sd_probe(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.sd_probe(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_http_probe(args: argparse.Namespace) -> int:
    """Run one isolated M4HttpTransport / WeRead step (base debugger)."""
    c = _open_client(args, ready_timeout=_resolve_ready_timeout(args))
    try:
        def on_prg(p: dict) -> None:
            phase = p.get("phase") or p.get("step") or ""
            if phase and phase != "host_wait":
                print(f"[http_probe] progress {json.dumps(p, ensure_ascii=False)}", flush=True)

        res = c.http_probe(
            args.step,
            host=getattr(args, "host", "weread.qq.com") or "weread.qq.com",
            url=getattr(args, "url", "https://weread.qq.com/") or "https://weread.qq.com/",
            session=not getattr(args, "no_session", False),
            book_id=getattr(args, "book_id", "") or "",
            chapter_uid=getattr(args, "chapter_uid", "") or "",
            psvts=getattr(args, "psvts", "") or "",
            app_id=getattr(args, "app_id", "com.weread.client") or "com.weread.client",
            wr_vid=getattr(args, "wr_vid", "") or "",
            wr_skey=getattr(args, "wr_skey", "") or "",
            wr_rt=getattr(args, "wr_rt", "") or "",
            timeout_ms=int(getattr(args, "timeout_ms", 30000)),
            timeout=float(getattr(args, "probe_timeout", 90.0)),
            on_progress=on_prg,
        )
        print(json.dumps(res, ensure_ascii=False, indent=2))
        return 0 if res.get("ok", False) or args.step in ("mem", "debug_on", "debug_off", "session_end", "shutdown") else 2
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_sd_read(args: argparse.Namespace) -> int:
    """Pull a small SD file slice over USB (plugin error.log etc.)."""
    import base64

    c = _open_client(args)
    try:
        res = c.sd_read(args.path, offset=args.offset, max_bytes=args.max)
        # Decode for human-readable stdout; raw JSON also available via --json.
        if getattr(args, "json", False):
            print(json.dumps(res, ensure_ascii=False, indent=2))
            return 0
        b64 = res.get("data_b64") or ""
        raw = base64.b64decode(b64) if b64 else b""
        meta = (
            f"# path={res.get('path')} size={res.get('size')} "
            f"offset={res.get('offset')} n={res.get('n')} eof={res.get('eof')}\n"
        )
        sys.stdout.write(meta)
        try:
            sys.stdout.write(raw.decode("utf-8", errors="replace"))
        except Exception:
            sys.stdout.buffer.write(raw)
        if raw and not raw.endswith(b"\n"):
            sys.stdout.write("\n")
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_wifi_status(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.wifi_status(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_m4b3_status(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.m4b3_status(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_wifi_prepare(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        result = c.wifi_prepare(timeout=max(5.0, float(getattr(args, "wifi_timeout", 45.0))))
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if not result.get("ready", result.get("connected", False)):
            print("设备 Wi-Fi 尚未就绪：请在设备网络设置保存网络后重试。", file=sys.stderr)
            return 2
        print(f"Wi-Fi 文件传输地址：{result.get('url') or '(无 IP)'}", flush=True)
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_wifi_transfer(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        result = c.wifi_transfer(timeout=max(5.0, float(getattr(args, "wifi_timeout", 45.0))))
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if result.get("ready"):
            print(f"已打开设备文件传输界面：{result.get('url') or '(无 IP)'}", flush=True)
            return 0
        print("已打开网络选择界面，请在设备上选择 Wi-Fi。", flush=True)
        return 2
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_install(args: argparse.Namespace) -> int:
    commit_to, overall_to, ready_to = _install_timeouts(args)
    c = _open_client(args, ready_timeout=ready_to)
    try:
        path = Path(args.path)
        if path.is_dir():
            m4x, _, _ = pkg.resolve_package(path, DEFAULT_CACHE)
        else:
            m4x = path
        transport = getattr(args, "transport", "auto") or "auto"
        print(
            f"[m4adb] 安装 {m4x.name} ({m4x.stat().st_size} bytes)；"
            f"transport={transport} commit≤{commit_to:.0f}s overall≤{overall_to:.0f}s",
            flush=True,
        )
        res = c.install(
            m4x,
            timeout=commit_to,
            overall_timeout=overall_to,
            progress=_install_progress,
            transport=transport,
        )
        print(json.dumps(res, ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_sync(args: argparse.Namespace) -> int:
    commit_to, overall_to, ready_to = _install_timeouts(args)
    c = _open_client(args, ready_timeout=ready_to)
    try:
        src = Path(args.source)
        m4x, ch, _ = pkg.resolve_package(src, DEFAULT_CACHE)
        state_path = DEFAULT_CACHE / "last_sync.json"
        state = json.loads(state_path.read_text()) if state_path.is_file() else {}
        key = str(src.resolve())
        # Never trust host cache as proof of device state: the cable may now be
        # connected to another M4, or the device may have rolled back. The
        # installer performs the authoritative content-hash no-op check.
        transport = getattr(args, "transport", "auto") or "auto"
        print(
            f"[m4adb] sync→install {m4x.name} ({m4x.stat().st_size} bytes)；"
            f"transport={transport} commit≤{commit_to:.0f}s overall≤{overall_to:.0f}s",
            flush=True,
        )
        res = c.install(
            m4x,
            timeout=commit_to,
            overall_timeout=overall_to,
            progress=_install_progress,
            transport=transport,
        )
        state[key] = ch
        state_path.parent.mkdir(parents=True, exist_ok=True)
        state_path.write_text(json.dumps(state, indent=2), encoding="utf-8")
        res = dict(res)
        res["content_hash"] = ch
        print(json.dumps(res, ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_launch(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.launch(args.app_id), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_tap(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.tap(args.x, args.y), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_swipe(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.swipe(args.sx, args.sy, args.ex, args.ey), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_key(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.key(args.name), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_back(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        print(json.dumps(c.back(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_screenshot(args: argparse.Namespace) -> int:
    c = _open_client(args)
    try:
        res = c.screenshot(Path(args.output))
        print(json.dumps(res, ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_ui(args: argparse.Namespace) -> int:
    """Dump structured UI text/state for automation (no OCR)."""
    c = _open_client(args)
    try:
        print(json.dumps(c.ui(), ensure_ascii=False, indent=2))
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


def cmd_logs(args: argparse.Namespace) -> int:
    c = _open_client(args)
    print("捕获串口日志（Ctrl-C 结束）…", flush=True)
    try:
        while True:
            data = c.t.read(0.2)
            if data:
                for line in data.splitlines():
                    print(f"{time.strftime('%H:%M:%S')} {line}")
    except KeyboardInterrupt:
        print("\n已停止")
        return 0
    finally:
        c.close()


def cmd_shell(args: argparse.Namespace) -> int:
    """ADB-like interactive shell backed by one persistent CDC connection."""
    c = _open_client(args)
    print("m4adb 常驻会话已连接。输入 help 查看命令，quit 退出。", flush=True)
    try:
        while True:
            try:
                parts = shlex.split(input("m4adb> "))
            except EOFError:
                break
            if not parts:
                continue
            op = parts[0].lower()
            if op in ("quit", "exit"):
                break
            if op == "help":
                print(
                    "ping | status | wifi_status | wifi_prepare | wifi_transfer | sd_probe | "
                    "sd_read <path> | "
                    "install <m4x|源码目录> | sync <源码目录> | "
                    "launch <app_id> | screenshot <pbm> | tap <x> <y> | "
                    "key <name> | back | home | quit"
                )
                continue
            try:
                if op == "ping" and len(parts) == 1:
                    result = c.ping()
                elif op == "status" and len(parts) == 1:
                    result = c.status()
                elif op == "wifi_status" and len(parts) == 1:
                    result = c.wifi_status()
                elif op == "wifi_prepare" and len(parts) == 1:
                    result = c.wifi_prepare(timeout=45.0)
                elif op == "wifi_transfer" and len(parts) == 1:
                    result = c.wifi_transfer(timeout=45.0)
                elif op == "sd_probe" and len(parts) == 1:
                    result = c.sd_probe()
                elif op in ("install", "sync") and len(parts) == 2:
                    src = Path(parts[1])
                    m4x, content_hash, _ = pkg.resolve_package(src, DEFAULT_CACHE)
                    state_path = DEFAULT_CACHE / "last_sync.json"
                    state = json.loads(state_path.read_text(encoding="utf-8")) if state_path.is_file() else {}
                    key = str(src.resolve())
                    # Device-side install is authoritative and returns noop
                    # when this exact content hash is already installed.
                    # Shell keeps one CDC owner; use long overall budget so
                    # e-ink-blocked devices still finish commit.
                    print(
                        f"[m4adb] {op} {m4x.name} ({m4x.stat().st_size} bytes) "
                        f"overall≤300s…",
                        flush=True,
                    )
                    result = dict(
                        c.install(
                            m4x,
                            timeout=180.0,
                            overall_timeout=300.0,
                            progress=_install_progress,
                            transport="auto",
                        )
                    )
                    state[key] = content_hash
                    state_path.parent.mkdir(parents=True, exist_ok=True)
                    state_path.write_text(json.dumps(state, indent=2), encoding="utf-8")
                    result["content_hash"] = content_hash
                elif op == "launch" and len(parts) == 2:
                    result = c.launch(parts[1])
                elif op == "screenshot" and len(parts) == 2:
                    result = c.screenshot(Path(parts[1]))
                elif op == "tap" and len(parts) == 3:
                    result = c.tap(int(parts[1]), int(parts[2]))
                elif op == "key" and len(parts) == 2:
                    result = c.key(parts[1])
                elif op == "back" and len(parts) == 1:
                    result = c.back()
                elif op == "home" and len(parts) == 1:
                    result = c.home()
                else:
                    print("命令或参数错误；输入 help 查看格式。", file=sys.stderr)
                    continue
                print(json.dumps(result, ensure_ascii=False, indent=2))
            except (BridgeError, ValueError) as e:
                print(f"错误: {e}", file=sys.stderr)
    finally:
        c.close()
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    journey = load_journey(Path(args.journey))
    if args.mock:
        # Seed mock with weread package for install/launch journeys
        from m4adb_lib.mock_device import MockDevice
        from m4adb_lib.transport import MockTransport

        dev = MockDevice()
        c = Client(MockTransport(dev), default_timeout=args.timeout)
    else:
        c = _open_client(args)
    try:
        rc, run_dir, report = run_journey(
            c,
            journey,
            Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS,
            DEFAULT_CACHE,
            repo_root=ROOT,
        )
        print(f"run_dir={run_dir}")
        print(f"ok={report.get('ok')}")
        return rc
    finally:
        c.close()


def cmd_watch(args: argparse.Namespace) -> int:
    journey_path = Path(args.journey)
    source = Path(args.source)
    persistent_client = None if (args.mock or args.once) else _open_client(args)

    def on_change(src: Path, ch: str) -> None:
        print(f"[watch] change hash={ch[:12]}… running journey", flush=True)
        if args.mock or args.once:
            from m4adb_lib.mock_device import MockDevice
            from m4adb_lib.transport import MockTransport

            dev = MockDevice()
            # Pre-install empty so sync can install
            c = Client(MockTransport(dev), default_timeout=args.timeout)
        else:
            # One long-lived CDC owner: do not reset the M4 for every rebuild.
            c = persistent_client
        try:
            j = load_journey(journey_path)
            rc, run_dir, report = run_journey(
                c, j, DEFAULT_ARTIFACTS, DEFAULT_CACHE, repo_root=ROOT
            )
            print(f"[watch] rc={rc} dir={run_dir} ok={report.get('ok')}", flush=True)
        finally:
            if args.mock or args.once:
                c.close()

    try:
        watch_loop(
            source,
            on_change,
            poll_s=float(args.poll),
            debounce_s=float(args.debounce),
            once=bool(args.once),
        )
        return 0
    except KeyboardInterrupt:
        print("\nwatch 已停止")
        return 0
    finally:
        if persistent_client is not None:
            persistent_client.close()


def cmd_doctor(args: argparse.Namespace) -> int:
    print("=== m4adb doctor ===")
    ok = True
    # Python
    print(f"[OK] Python {sys.version.split()[0]}")
    # pyserial
    try:
        import serial  # noqa: F401

        print("[OK] pyserial 已安装")
    except ImportError:
        print("[WARN] 未安装 pyserial（仅 mock/--mock 可跑；真机需要: pip install pyserial）")
        if not args.mock:
            ok = False
    # port
    if args.mock:
        print("[OK] --mock 模式，跳过串口")
        c = Client(MockTransport(MockDevice()))
        try:
            st = c.ping()
            print(f"[OK] 握手成功 protocol={st.get('protocol')} firmware={st.get('firmware')}")
            if not st.get("sd_ok", True):
                print("[WARN] SD 状态异常")
        finally:
            c.close()
        print("诊断完成（mock）")
        return 0

    port = args.port or auto_port()
    if not port:
        print("[FAIL] 未找到串口。macOS 检查 /dev/cu.usbmodem*；Linux 检查 /dev/ttyACM* 与 dialout 组权限。")
        print("  修复: sudo usermod -aG dialout $USER  然后重新登录")
        return 1
    print(f"[OK] 使用串口 {port}（常驻会话优先）")
    try:
        c = _open_client(args)
    except Exception as e:
        print(f"[FAIL] 打开调试会话失败: {e}")
        print("  请确认设备已连接，且没有其他程序占用串口；不要意外触发 DTR 复位。")
        return 1
    try:
        st = c.ping()
        print(f"[OK] 握手成功 protocol={st.get('protocol')} firmware={st.get('firmware')}")
        print(f"     activity={st.get('activity')} free_heap={st.get('free_heap')} sd_ok={st.get('sd_ok')}")
        if st.get("protocol") != 1:
            print("[WARN] 协议版本不匹配，请升级 m4adb 或固件")
            ok = False
        if not st.get("sd_ok", True):
            print("[FAIL] SD 不可用，安装/同步会失败")
            ok = False
    except BridgeError as e:
        print(f"[FAIL] 握手失败 {e.key}: {e.message}")
        print("  确认设备已在「设置 → 系统 → 开发者选项」中开启「USB 串口调试」，")
        print("  且串口波特率 115200、无其他监视器占用。")
        ok = False
    finally:
        c.close()
    print("诊断完成：" + ("通过" if ok else "存在问题"))
    return 0 if ok else 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="m4adb", description="Murphy M4 USB serial debug automation")
    p.add_argument("--port", default=None, help="串口路径（默认自动检测）")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--no-daemon",
        action="store_true",
        help="不复用常驻 USB 会话（仅故障排查；可能触发设备复位）",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="通用请求超时（秒）。install/sync 就绪等待见 --ready-timeout",
    )
    p.add_argument(
        "--ready-timeout",
        type=float,
        default=None,
        help="串口 bridge 就绪等待（秒）。默认 max(15, --timeout)；显式指定可小于 15 以便冒烟快速失败",
    )
    p.add_argument("--mock", action="store_true", help="使用主机 mock 设备（无硬件）")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("devices", help="列出候选串口")
    sub.add_parser("ping", help="握手")
    sub.add_parser("status", help="状态快照")
    sub.add_parser("sd_probe", help="SD 卡写入/同步/读取/删除探针")
    php = sub.add_parser(
        "http_probe",
        help="M4HttpTransport 底座分步调试（mem/session/tls_get/weread_psvts/e0…）",
    )
    php.add_argument(
        "step",
        help="mem|debug_on|debug_off|session_begin|session_end|tls_get|"
        "weread_psvts|weread_e0|weread_t0|weread_t1|weread_e1|weread_e3|shutdown",
    )
    php.add_argument("--host", default="weread.qq.com", help="session_begin 主机名")
    php.add_argument("--url", default="https://weread.qq.com/", help="tls_get URL")
    php.add_argument("--no-session", action="store_true", help="强制 oneshot（不走 session）")
    php.add_argument("--book-id", default="", help="微信读书 bookId")
    php.add_argument("--chapter-uid", default="", help="章节 uid")
    php.add_argument("--psvts", default="", help="可选：覆盖设备内缓存的 psvts")
    php.add_argument("--app-id", default="com.weread.client")
    php.add_argument("--wr-vid", default="", help="weread_set_cookie: wr_vid")
    php.add_argument("--wr-skey", default="", help="weread_set_cookie: wr_skey")
    php.add_argument("--wr-rt", default="", help="weread_set_cookie: wr_rt")
    php.add_argument("--timeout-ms", type=int, default=30000, help="设备侧 HTTP 超时")
    php.add_argument(
        "--probe-timeout",
        type=float,
        default=90.0,
        help="主机等待该步完成的超时（秒，默认 90）",
    )
    psr = sub.add_parser(
        "sd_read",
        help="经 USB 读取 SD 文件片段（仅 apps_data/apps_inbox，默认 tail 400B）",
    )
    psr.add_argument(
        "path",
        help="如 apps_data/com.jjwxc.client/logs/error.log",
    )
    psr.add_argument(
        "--offset",
        type=int,
        default=-1,
        help="字节偏移；默认 -1=从文件尾部读",
    )
    psr.add_argument(
        "--max",
        type=int,
        default=400,
        help="最多读取字节（设备上限 400）",
    )
    psr.add_argument(
        "--json",
        action="store_true",
        help="输出原始 JSON（含 data_b64）",
    )
    pi = sub.add_parser("install", help="安装 .m4x 或源目录（单次连接，含整体超时）")
    pi.add_argument("path")
    pi.add_argument(
        "--ready-timeout",
        type=float,
        default=60.0,
        help="打开串口后等待桥就绪最长时间（秒，默认 60）",
    )
    pi.add_argument(
        "--commit-timeout",
        type=float,
        default=180.0,
        help="install_commit（设备解压/写 SD）超时（秒，默认 180）",
    )
    pi.add_argument(
        "--overall-timeout",
        type=float,
        default=300.0,
        help="begin+上传+commit 总墙钟超时（秒，默认 300）",
    )
    pi.add_argument(
        "--transport",
        choices=("wifi", "auto", "usb"),
        default="auto",
        help="传包通道：auto=优先 Wi-Fi，失败回退 USB；wifi=仅局域网 HTTP；usb=仅串口分片",
    )
    ps = sub.add_parser("sync", help="仅在内容变化时安装")
    ps.add_argument("source")
    ps.add_argument("--force", action="store_true")
    ps.add_argument("--ready-timeout", type=float, default=60.0)
    ps.add_argument("--commit-timeout", type=float, default=180.0)
    ps.add_argument("--overall-timeout", type=float, default=300.0)
    ps.add_argument(
        "--transport",
        choices=("wifi", "auto", "usb"),
        default="auto",
        help="同 install --transport（默认 auto：Wi-Fi 失败回退 USB）",
    )
    pws = sub.add_parser("wifi_status", help="读取设备 Wi-Fi/IP 状态（不改动连接）")
    sub.add_parser("m4b3_status", help="读取 M4B3 TCP 接收器状态（逻辑 framebuffer / ACK）")
    pwp = sub.add_parser("wifi_prepare", help="通过当前 USB 会话激活已保存 Wi-Fi")
    pwp.add_argument("--wifi-timeout", type=float, default=45.0)
    pwt = sub.add_parser("wifi_transfer", help="USB 激活 Wi-Fi 并打开设备文件传输界面")
    pwt.add_argument("--wifi-timeout", type=float, default=45.0)
    pd = sub.add_parser("daemon", help="启动常驻 USB 调试服务（通常由 m4adb 自动启动）")
    pd.add_argument("--socket", default=None, help="Unix socket 路径（默认按串口生成）")
    pd.add_argument("--ready-timeout", type=float, default=60.0)
    sub.add_parser("daemon_stop", help="停止常驻 USB 调试服务（不触碰设备数据）")
    pl = sub.add_parser("launch", help="启动已安装应用")
    pl.add_argument("app_id")
    pt = sub.add_parser("tap", help="注入点击（逻辑坐标）")
    pt.add_argument("x", type=int)
    pt.add_argument("y", type=int)
    psw = sub.add_parser("swipe", help="注入滑动（逻辑起止坐标）")
    psw.add_argument("sx", type=int)
    psw.add_argument("sy", type=int)
    psw.add_argument("ex", type=int)
    psw.add_argument("ey", type=int)
    pk = sub.add_parser("key", help="注入按键")
    pk.add_argument("name")
    sub.add_parser("back", help="注入 Back")
    pss = sub.add_parser("screenshot", help="导出逻辑 PBM 截图")
    pss.add_argument("output")
    sub.add_parser(
        "ui",
        help="导出结构化界面状态 JSON（activity/screen/error/list，自动化优先用这个，无需 OCR）",
    )
    sub.add_parser("logs", help="带时间戳捕获串口日志")
    sub.add_parser("shell", help="常驻串口交互会话（避免逐命令重连复位）")
    pr = sub.add_parser("run", help="执行 journey JSON")
    pr.add_argument("journey")
    pr.add_argument("--artifacts", default=None)
    pw = sub.add_parser("watch", help="监视源并在变化时跑 journey")
    pw.add_argument("source")
    pw.add_argument("--journey", required=True)
    pw.add_argument("--poll", default="1.0")
    pw.add_argument("--debounce", default="0.8")
    pw.add_argument("--once", action="store_true", help="只跑一轮（CI/无设备）")
    sub.add_parser("doctor", help="环境诊断（中文）")
    return p


def main(argv: list[str] | None = None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    parser = build_parser()
    args = parser.parse_args(argv)
    cmds = {
        "daemon": cmd_daemon,
        "daemon_stop": cmd_daemon_stop,
        "devices": cmd_devices,
        "ping": cmd_ping,
        "status": cmd_status,
        "sd_probe": cmd_sd_probe,
        "http_probe": cmd_http_probe,
        "sd_read": cmd_sd_read,
        "wifi_status": cmd_wifi_status,
        "m4b3_status": cmd_m4b3_status,
        "wifi_prepare": cmd_wifi_prepare,
        "wifi_transfer": cmd_wifi_transfer,
        "install": cmd_install,
        "sync": cmd_sync,
        "launch": cmd_launch,
        "tap": cmd_tap,
        "swipe": cmd_swipe,
        "key": cmd_key,
        "back": cmd_back,
        "screenshot": cmd_screenshot,
        "ui": cmd_ui,
        "logs": cmd_logs,
        "shell": cmd_shell,
        "run": cmd_run,
        "watch": cmd_watch,
        "doctor": cmd_doctor,
    }
    return cmds[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
