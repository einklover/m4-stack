# -*- coding: utf-8 -*-
"""Interactive Murphy M4 debug shell for humans and AI agents.

Goals
-----
* One persistent m4adb serial connection per session.  No repeated open/close
  that can reset the ESP32-S3 or fight the UI viewer for the PTY.
* Structured commands for the simulator workflow: status, ui, launch, tap, key,
  sd_probe, sd_read, install, wait, watch, logs, qemu/restart, viewer.
* Safe for AI batch use: --execute or --file runs commands in one process and
  one serial session, and every command has a wall-clock timeout.
* Raw serial lines are kept in memory and appended to build/m4dbg/serial.log
  so ESP-IDF errors such as sdmmc_wait_for_idle timeout are easy to grep.

Example
-------
    python3 simulator/tools/m4dbg.py --port /dev/ttys038
    python3 simulator/tools/m4dbg.py --execute "status; ui; launch com.jjwxc.client; wait app com.jjwxc.client 30; ui; logs 80"
"""

from __future__ import annotations

import argparse
import cmd as _cmd
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_SCRIPTS = ROOT / "firmware" / "scripts"
M4SIM = ROOT / "simulator" / "m4sim.py"
STATE = Path(os.environ.get("M4SIM_TMP", "/tmp/m4sim")) / "state.json"
LOG_DIR = ROOT / "build" / "m4dbg"
SERIAL_LOG = LOG_DIR / "serial.log"

sys.path.insert(0, str(FIRMWARE_SCRIPTS))

from m4adb_lib.client import BridgeError, Client  # noqa: E402
from m4adb_lib.transport import SerialTransport  # noqa: E402


class CmdError(RuntimeError):
    pass


def now() -> str:
    return time.strftime("%H:%M:%S")


def read_m4sim_state() -> dict[str, Any]:
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def m4sim(*args: str, timeout: float = 30) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(M4SIM), *args]
    return subprocess.run(
        cmd, cwd=ROOT, timeout=timeout, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )


class Session:
    """Persistent m4adb serial session with built-in logging."""

    def __init__(self, port: str, baud: int = 115200, default_timeout: float = 8.0):
        self.port = port
        self.baud = baud
        self.default_timeout = default_timeout
        self.client: Client | None = None
        self._recent: list[str] = []
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        self._log_file = SERIAL_LOG.open("a", encoding="utf-8")
        self._connect()

    def _connect(self) -> None:
        if self.client is not None:
            self._close_client()
        self.client = Client(SerialTransport(self.port, self.baud), default_timeout=self.default_timeout)
        self.client.wait_ready(timeout=max(8.0, self.default_timeout))

    def _close_client(self) -> None:
        if self.client is not None:
            try:
                self.client.close()
            except Exception:
                pass
            self.client = None

    def close(self) -> None:
        self._close_client()
        try:
            self._log_file.close()
        except Exception:
            pass

    def request(self, op: str, obj: dict | None = None, timeout: float | None = None) -> dict:
        if self.client is None:
            raise CmdError("no client connected")
        to = timeout or self.default_timeout
        try:
            return self.client.request({"op": op, **(obj or {})}, timeout=to)
        except BridgeError as e:
            raise CmdError(f"{e.key}: {e.message}") from e

    def _append_logs(self) -> None:
        if self.client is None:
            return
        lines = getattr(self.client, "serial_log", [])
        for line in lines:
            self._log_file.write(f"{now()} {line}\n")
        self._log_file.flush()
        if lines:
            self._recent.extend(lines)
            del lines[:]

    def _recent_snapshot(self) -> list[str]:
        if self.client is not None:
            self._append_logs()
        return self._recent[-200:]

    def status(self, timeout: float | None = None) -> dict:
        return self.request("status", timeout=timeout)

    def ui(self, timeout: float | None = None) -> dict:
        return self.request("ui", timeout=timeout)

    def ping(self, timeout: float | None = None) -> dict:
        return self.request("ping", timeout=timeout)

    def launch(self, app_id: str, timeout: float | None = 20.0) -> dict:
        return self.request("launch", {"app_id": app_id}, timeout=timeout)

    def tap(self, x: int, y: int, timeout: float | None = None) -> dict:
        return self.request("tap", {"x": int(x), "y": int(y)}, timeout=timeout)

    def key(self, name: str, timeout: float | None = None) -> dict:
        aliases = {"enter": "confirm", "ok": "confirm", "confirm": "confirm", "back": "back",
                   "left": "left", "right": "right", "up": "up", "down": "down"}
        mapped = aliases.get(name.lower(), name)
        return self.request("key", {"name": mapped}, timeout=timeout)

    def back(self, timeout: float | None = None) -> dict:
        return self.request("back", timeout=timeout)

    def home(self, timeout: float | None = None) -> dict:
        return self.request("home", timeout=timeout)

    def sd_probe(self, timeout: float | None = 15.0) -> dict:
        return self.request("sd_probe", timeout=timeout)

    def sd_read(self, path: str, timeout: float | None = None) -> dict:
        return self.request("sd_read", {"path": path}, timeout=timeout)

    def wifi_status(self, timeout: float | None = None) -> dict:
        return self.request("wifi_status", timeout=timeout)

    def wait_app(self, app_id: str, timeout: float = 30.0, interval: float = 2.0) -> dict:
        deadline = time.time() + timeout
        last: dict = {}
        while time.time() < deadline:
            last = self.status(timeout=min(5.0, max(1.0, deadline - time.time())))
            if last.get("active_app") == app_id:
                return last
            time.sleep(interval)
        raise CmdError(f"wait_app {app_id} timed out; last active_app={last.get('active_app')}")

    def wait_activity(self, activity: str, timeout: float = 30.0, interval: float = 2.0) -> dict:
        deadline = time.time() + timeout
        last: dict = {}
        while time.time() < deadline:
            last = self.status(timeout=min(5.0, max(1.0, deadline - time.time())))
            if last.get("activity") == activity:
                return last
            time.sleep(interval)
        raise CmdError(f"wait_activity {activity} timed out; last activity={last.get('activity')}")

    def install_package(self, path: str) -> dict:
        from m4adb_lib import package as pkg
        src = Path(path).expanduser().resolve()
        if src.is_dir():
            m4x, _, _ = pkg.resolve_package(src, LOG_DIR)
        else:
            m4x = src
        if self.client is None:
            raise CmdError("no client connected")
        print(f"installing {m4x.name} ({m4x.stat().st_size} bytes)", flush=True)
        try:
            return self.client.install_usb(
                m4x, timeout=180, overall_timeout=300,
                progress=lambda done, total, phase: print(f"  {phase} {done}/{total}", flush=True),
            )
        except BridgeError as e:
            raise CmdError(f"{e.key}: {e.message}") from e

    def watch_serial(self, seconds: float) -> None:
        if self.client is None:
            raise CmdError("no client connected")
        deadline = time.time() + seconds
        while time.time() < deadline:
            data = self.client.t.read(0.2)
            if data:
                for line in data.splitlines():
                    print(f"{now()} {line}", flush=True)
                    self._log_file.write(f"{now()} {line}\n")
                    self._recent.append(line)
            else:
                time.sleep(0.05)
        self._log_file.flush()


class M4DbgShell(_cmd.Cmd):
    intro = "Murphy M4 debug shell. Type help or ? to list commands.\n"
    prompt = "m4dbg> "

    def __init__(self, sess: Session, batch: bool = False, keep_going: bool = False):
        super().__init__(stdout=sys.stdout)
        self.sess = sess
        self.batch = batch
        self.keep_going = keep_going
        self.last_error: str | None = None
        self.last_status: dict = {}

    def _print_json(self, obj: Any) -> None:
        print(json.dumps(obj, ensure_ascii=False, indent=2)[:4000], flush=True)

    def _run(self, fn: Callable, *args: Any, **kwargs: Any) -> bool:
        self.last_error = None
        try:
            res = fn(*args, **kwargs)
            if isinstance(res, dict):
                self._print_json(res)
                self.last_status = res
            elif isinstance(res, str):
                print(res, flush=True)
            self._show_new_logs()
            return True
        except (CmdError, BridgeError) as e:
            self.last_error = str(e)
            print(f"ERROR {e}", file=sys.stderr, flush=True)
            self._show_new_logs()
            return False
        except Exception as e:
            self.last_error = f"{type(e).__name__}: {e}"
            print(f"ERROR {self.last_error}", file=sys.stderr, flush=True)
            self._show_new_logs()
            return False

    def _show_new_logs(self) -> None:
        if self.batch:
            recent = self.sess._recent_snapshot()
            if recent:
                print(f"--- serial ({len(recent)} recent) ---", flush=True)
                for line in recent[-30:]:
                    print(line, flush=True)

    def do_status(self, arg: str) -> None:
        """status - one m4adb status snapshot."""
        self._run(self.sess.status)

    def do_ui(self, arg: str) -> None:
        """ui - structured UI dump (activity + body)."""
        self._run(self.sess.ui)

    def do_ping(self, arg: str) -> None:
        """ping - protocol handshake."""
        self._run(self.sess.ping)

    def do_launch(self, arg: str) -> None:
        """launch <app_id> - launch a plugin."""
        app_id = arg.strip()
        if not app_id:
            print("usage: launch <app_id>", file=sys.stderr)
            return
        self._run(self.sess.launch, app_id)

    def do_tap(self, arg: str) -> None:
        """tap <x> <y> - inject tap at logical coordinates."""
        parts = arg.split()
        if len(parts) != 2:
            print("usage: tap <x> <y>", file=sys.stderr)
            return
        try:
            x, y = int(parts[0]), int(parts[1])
        except ValueError:
            print("usage: tap <x> <y>", file=sys.stderr)
            return
        self._run(self.sess.tap, x, y)

    def do_key(self, arg: str) -> None:
        """key <name> - inject key (back/enter/left/right)."""
        name = arg.strip()
        if not name:
            print("usage: key <name>", file=sys.stderr)
            return
        self._run(self.sess.key, name)

    def do_back(self, arg: str) -> None:
        """back - inject Back."""
        self._run(self.sess.back)

    def do_home(self, arg: str) -> None:
        """home - navigate Home."""
        self._run(self.sess.home)

    def do_sd_probe(self, arg: str) -> None:
        """sd_probe - write/read/delete SD probe file."""
        self._run(self.sess.sd_probe)

    def do_sd_read(self, arg: str) -> None:
        """sd_read <path> - read a small file from apps_data/apps_inbox."""
        path = arg.strip()
        if not path:
            print("usage: sd_read <path>", file=sys.stderr)
            return
        self._run(self.sess.sd_read, path)

    def do_wifi_status(self, arg: str) -> None:
        """wifi_status - read Wi-Fi status."""
        self._run(self.sess.wifi_status)

    def do_install(self, arg: str) -> None:
        """install <m4x|source-dir> - install a plugin package."""
        path = arg.strip()
        if not path:
            print("usage: install <path>", file=sys.stderr)
            return
        self._run(self.sess.install_package, path)

    def do_wait(self, arg: str) -> None:
        """wait app <app_id> [timeout] | wait activity <name> [timeout]."""
        parts = arg.split()
        if len(parts) >= 2:
            kind, value = parts[0], parts[1]
            try:
                timeout = float(parts[2]) if len(parts) >= 3 else 30.0
            except ValueError:
                timeout = 30.0
            if kind == "app":
                self._run(self.sess.wait_app, value, timeout)
                return
            if kind == "activity":
                self._run(self.sess.wait_activity, value, timeout)
                return
        print("usage: wait app <app_id> [timeout] | wait activity <name> [timeout]", file=sys.stderr)

    def do_sleep(self, arg: str) -> None:
        """sleep <seconds> - wait before the next batch command."""
        try:
            seconds = float(arg.strip()) if arg.strip() else 1.0
        except ValueError:
            print("usage: sleep <seconds>", file=sys.stderr)
            return
        time.sleep(seconds)
        print(f"slept {seconds:.1f}s", flush=True)

    def do_watch(self, arg: str) -> None:
        """watch [seconds] - passively print raw serial for a while (default 5s)."""
        try:
            seconds = float(arg.strip()) if arg.strip() else 5.0
        except ValueError:
            print("usage: watch [seconds]", file=sys.stderr)
            return
        self._run(self.sess.watch_serial, seconds)

    def do_logs(self, arg: str) -> None:
        """logs [n] - show recent serial log lines (default 80)."""
        try:
            n = int(arg.strip()) if arg.strip() else 80
        except ValueError:
            print("usage: logs [n]", file=sys.stderr)
            return
        recent = self.sess._recent_snapshot()
        for line in recent[-n:]:
            print(line, flush=True)

    def do_qemu(self, arg: str) -> None:
        """qemu - print m4sim info (pid, pty, running)."""
        cp = m4sim("info")
        print(cp.stdout, end="", flush=True)

    def do_restart(self, arg: str) -> None:
        """restart [ restart QEMU through m4sim and reconnect the serial session."""
        fresh = "--fresh-sd" if arg.strip() == "fresh" else ""
        print("stopping m4sim ", flush=True)
        cp = m4sim("stop")
        print(cp.stdout, end="", flush=True)
        cmd = [sys.executable, str(M4SIM), "run", "--plugin-debug", "--skip-build", "--ready-seconds", "120"]
        if fresh:
            cmd.append(fresh)
        print("+", shlex.join(cmd), flush=True)
        cp = subprocess.run(cmd, cwd=ROOT, timeout=150, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(cp.stdout, end="", flush=True)
        if cp.returncode != 0:
            print("restart failed", file=sys.stderr)
            return
        st = read_m4sim_state()
        new_pty = st.get("pty")
        if not new_pty:
            print("no pty in m4sim state", file=sys.stderr)
            return
        print(f"reconnecting to {new_pty}", flush=True)
        self.sess.port = new_pty
        self.sess._connect()
        self._run(self.sess.ping)

    def do_viewer(self, arg: str) -> None:
        """viewer - open m4_screen_viewer for the current m4sim session."""
        subprocess.Popen(
            [sys.executable, str(M4SIM), "ui"],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        print("viewer launched", flush=True)

    def do_q(self, arg: str) -> bool:
        """q - quit."""
        print("bye", flush=True)
        return True

    def do_EOF(self, arg: str) -> bool:
        print("", flush=True)
        return True


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial PTY path; default reads /tmp/m4sim/state.json")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=8.0, help="default request timeout seconds")
    p.add_argument("--execute", help="run semicolon-separated commands and exit")
    p.add_argument("--file", help="run commands from a file and exit")
    p.add_argument("--keep-going", action="store_true", help="continue after command errors")
    return p


def _resolve_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    st = read_m4sim_state()
    pty = st.get("pty")
    if pty:
        return str(pty)
    raise SystemExit("no --port and no /tmp/m4sim/state.json pty found")


def _split_commands(script: str) -> list[str]:
    out: list[str] = []
    for raw in script.replace("\n", ";").split(";"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if " #" in line:
            line = line.split(" #", 1)[0].rstrip()
        out.append(line)
    return out


def main() -> int:
    args = _build_parser().parse_args()
    port = _resolve_port(args.port)
    sess = Session(port, baud=args.baud, default_timeout=args.timeout)
    shell = M4DbgShell(sess, batch=bool(args.execute or args.file), keep_going=args.keep_going)
    rc = 0
    try:
        if args.execute:
            for line in _split_commands(args.execute):
                print(f"\n=== m4dbg> {line} ===", flush=True)
                ok = shell.onecmd(line)
                if not ok and not args.keep_going:
                    rc = 1
                    break
        elif args.file:
            for line in _split_commands(Path(args.file).read_text(encoding="utf-8")):
                print(f"\n=== m4dbg> {line} ===", flush=True)
                ok = shell.onecmd(line)
                if not ok and not args.keep_going:
                    rc = 1
                    break
        else:
            shell.cmdloop()
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        sess.close()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
