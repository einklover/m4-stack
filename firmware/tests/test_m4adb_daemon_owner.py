#!/usr/bin/env python3
"""Single-owner m4adb: flock, attach policy, mux, no stop-on-timeout."""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import m4adb  # noqa: E402
from m4adb_lib import package as pkg  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402
from m4adb_lib.daemon import (  # noqa: E402
    BridgeDaemon,
    DaemonTransport,
    should_reopen_serial,
)
from m4adb_lib.owner import (  # noqa: E402
    ExclusivePortLock,
    decide_attach,
    lock_is_held,
    owner_paths,
)
from m4adb_lib.transport import is_hardware_port, port_node_present  # noqa: E402


class DecideAttachTests(unittest.TestCase):
    def test_table(self):
        cases = [
            (True, True, False, "reuse"),
            (True, False, False, "reuse"),
            (False, True, False, "wait"),
            (False, False, False, "spawn"),
            (True, True, True, "refuse"),
            (True, False, True, "refuse"),
            (False, True, True, "refuse"),
            (False, False, True, "direct"),
        ]
        for sock, held, no_daemon, expected in cases:
            self.assertEqual(
                decide_attach(socket_alive=sock, lock_held=held, no_daemon=no_daemon),
                expected,
                msg=(sock, held, no_daemon),
            )


class ShouldReopenTests(unittest.TestCase):
    def test_never_reopen_live_handle(self):
        self.assertFalse(
            should_reopen_serial(
                serial_open=True, port_present=True, last_attempt=0, now=10, delay=2
            )
        )

    def test_reopen_only_after_node_returns_and_backoff(self):
        self.assertFalse(
            should_reopen_serial(
                serial_open=False, port_present=False, last_attempt=0, now=10, delay=2
            )
        )
        self.assertFalse(
            should_reopen_serial(
                serial_open=False, port_present=True, last_attempt=9.0, now=10.0, delay=2
            )
        )
        self.assertTrue(
            should_reopen_serial(
                serial_open=False, port_present=True, last_attempt=0, now=10, delay=2
            )
        )


class PortHelpersTests(unittest.TestCase):
    def test_hardware_vs_qemu(self):
        self.assertTrue(is_hardware_port("/dev/cu.usbmodem101"))
        self.assertFalse(is_hardware_port("/dev/ttys046"))
        self.assertFalse(is_hardware_port("/tmp/m4uart.pipe"))

    def test_port_node_present_tmp(self):
        with tempfile.NamedTemporaryFile() as f:
            self.assertTrue(port_node_present(f.name))
        self.assertFalse(port_node_present("/no/such/m4adb-port-node"))


class FlockSubprocessTests(unittest.TestCase):
    def test_exclusive_lock_blocks_other_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            lock_path = tmp_path / "m4.lock"
            pid_path = tmp_path / "m4.pid"
            child = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    "from pathlib import Path\n"
                    "from m4adb_lib.owner import ExclusivePortLock\n"
                    "import sys, time\n"
                    "lock = ExclusivePortLock(Path(sys.argv[1]), Path(sys.argv[2]))\n"
                    "assert lock.try_acquire()\n"
                    "print('held', flush=True)\n"
                    "time.sleep(30)\n",
                    str(lock_path),
                    str(pid_path),
                ],
                cwd=str(ROOT / "scripts"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                line = child.stdout.readline() if child.stdout else ""
                self.assertIn("held", line)
                self.assertTrue(lock_is_held(lock_path))
                other = ExclusivePortLock(lock_path, pid_path)
                self.assertFalse(other.try_acquire())
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
                if child.stdout:
                    child.stdout.close()
                if child.stderr:
                    child.stderr.close()


class LoopbackSerial:
    def __init__(self):
        self.writes: list[str] = []
        self._rx = ""
        self._open = True
        self.pings = 0

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        self.writes.append(data)

    def read(self, timeout: float = 0.05) -> str:
        out = self._rx
        self._rx = ""
        return out

    def close(self) -> None:
        self._open = False

    def alive(self) -> bool:
        return self._open

    def inject(self, data: str) -> None:
        self._rx += data


class DaemonMuxTests(unittest.TestCase):
    def test_line_buffer_and_broadcast_no_startup_ping(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            port = str(tmp_path / "fake.port")
            Path(port).write_text("x")
            paths = owner_paths(port, tmp_path)
            serial = LoopbackSerial()
            lock = ExclusivePortLock(paths.lock, paths.pid)
            daemon = BridgeDaemon(
                port,
                115200,
                paths.socket,
                transport_factory=lambda: serial,
                lock=lock,
            )
            thread = threading.Thread(target=daemon.serve, daemon=True)
            thread.start()
            deadline = time.time() + 5
            while time.time() < deadline and not paths.socket.exists():
                time.sleep(0.05)
            self.assertTrue(paths.socket.exists())
            time.sleep(0.1)
            self.assertEqual(serial.writes, [])

            a = DaemonTransport(paths.socket, timeout=2)
            b = DaemonTransport(paths.socket, timeout=2)
            a.write("hello")  # no newline yet via write which adds \n
            time.sleep(0.15)
            self.assertTrue(any("hello" in w for w in serial.writes))

            serial.inject("from-device\n")
            time.sleep(0.2)
            ra = a.read(0.3)
            rb = b.read(0.3)
            self.assertIn("from-device", ra)
            self.assertIn("from-device", rb)

            second = BridgeDaemon(
                port,
                115200,
                paths.socket,
                transport_factory=lambda: LoopbackSerial(),
                lock=ExclusivePortLock(paths.lock, paths.pid),
            )
            self.assertEqual(second.serve(), 2)

            a.close()
            b.close()
            daemon.running = False
            thread.join(timeout=3)


class OpenClientPolicyTests(unittest.TestCase):
    def test_timeout_on_reuse_does_not_stop_or_spawn(self):
        args = argparse.Namespace(
            mock=False,
            no_daemon=False,
            port="/dev/cu.usbmodem-test",
            timeout=2,
            ready_timeout=1,
            baud=115200,
        )
        fake_paths = owner_paths(args.port)
        with mock.patch.object(m4adb, "owner_paths", return_value=fake_paths), mock.patch.object(
            m4adb, "daemon_alive", return_value=True
        ), mock.patch.object(m4adb, "lock_is_held", return_value=True), mock.patch.object(
            m4adb, "stop_daemon"
        ) as stop, mock.patch.object(
            m4adb, "_spawn_daemon"
        ) as spawn, mock.patch.object(
            m4adb, "_client_from_socket", side_effect=BridgeError("timeout", "wait")
        ):
            with self.assertRaises(BridgeError) as ctx:
                m4adb._open_client(args)
            self.assertEqual(ctx.exception.key, "timeout")
            stop.assert_not_called()
            spawn.assert_not_called()

    def test_usb_debug_off_does_not_spawn(self):
        args = argparse.Namespace(
            mock=False,
            no_daemon=False,
            port="/dev/cu.usbmodem-test",
            timeout=2,
            ready_timeout=1,
            baud=115200,
        )
        fake_paths = owner_paths(args.port)
        with mock.patch.object(m4adb, "owner_paths", return_value=fake_paths), mock.patch.object(
            m4adb, "daemon_alive", return_value=True
        ), mock.patch.object(m4adb, "lock_is_held", return_value=False), mock.patch.object(
            m4adb, "stop_daemon"
        ) as stop, mock.patch.object(
            m4adb, "_spawn_daemon"
        ) as spawn, mock.patch.object(
            m4adb,
            "_client_from_socket",
            side_effect=BridgeError("usb_debug_off", "请开启 USB 串口调试"),
        ):
            with self.assertRaises(BridgeError) as ctx:
                m4adb._open_client(args)
            self.assertEqual(ctx.exception.key, "usb_debug_off")
            stop.assert_not_called()
            spawn.assert_not_called()

    def test_no_daemon_refuses_when_owner_exists(self):
        args = argparse.Namespace(
            mock=False,
            no_daemon=True,
            port="/dev/cu.usbmodem-test",
            timeout=2,
            ready_timeout=1,
            baud=115200,
        )
        fake_paths = owner_paths(args.port)
        with mock.patch.object(m4adb, "owner_paths", return_value=fake_paths), mock.patch.object(
            m4adb, "daemon_alive", return_value=True
        ), mock.patch.object(m4adb, "lock_is_held", return_value=True):
            with self.assertRaises(SystemExit):
                m4adb._open_client(args)


class InstallCliTests(unittest.TestCase):
    def test_default_usb_and_launch_flag(self):
        parser = m4adb.build_parser()
        args = parser.parse_args(["install", "foo.m4x"])
        self.assertEqual(args.transport, "usb")
        self.assertFalse(args.launch)
        args = parser.parse_args(["install", "foo.m4x", "--launch", "--transport", "auto"])
        self.assertTrue(args.launch)
        self.assertEqual(args.transport, "auto")

    def test_package_app_id_dir_and_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "plugin"
            src.mkdir()
            (src / "manifest.json").write_text(
                json.dumps({"id": "com.m4.game2048", "entry": "main.lua"}),
                encoding="utf-8",
            )
            (src / "main.lua").write_text("--", encoding="utf-8")
            self.assertEqual(pkg.package_app_id(src), "com.m4.game2048")
            zpath = Path(tmp) / "p.m4x"
            with zipfile.ZipFile(zpath, "w") as zf:
                zf.write(src / "manifest.json", "manifest.json")
            self.assertEqual(pkg.package_app_id(zpath), "com.m4.game2048")


if __name__ == "__main__":
    unittest.main()
