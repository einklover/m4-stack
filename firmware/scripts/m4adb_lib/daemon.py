"""Persistent USB-Serial/JTAG bridge for m4adb.

The ESP32-S3 can reset when a CDC handle is opened repeatedly.  This module
keeps exactly one SerialTransport open and exposes a 0600 Unix socket; CLI
invocations use the socket and never reopen the hardware port.

Guarantees:
  * Single instance per port: a second `serve()` on the same socket path
    exits immediately unless the existing daemon is dead (stale socket).
  * Auto-reconnect: if the device resets or the USB handle dies, the daemon
    keeps serving the socket and periodically reopens the serial port with
    bounded backoff. Clients only ever see a momentary timeout, never a
    "daemon disconnected" that requires manual restart.
"""

from __future__ import annotations

import hashlib
import os
import select
import socket
import sys
import time
from pathlib import Path
from typing import Optional

from .transport import SerialTransport


def socket_path_for_port(port: str) -> Path:
    """Return a stable, per-port socket path without leaking the full path."""
    digest = hashlib.sha1(port.encode("utf-8", errors="replace")).hexdigest()[:12]
    return Path("/tmp") / f"m4adb-{digest}.sock"


def serial_port_alive(transport) -> bool:
    """Probe whether the underlying serial port still responds.  A transient
    e-ink refresh stall keeps the port open (in_waiting works, read may be
    empty); a removed/unplugged device raises immediately."""
    try:
        ser = getattr(transport, "_ser", None)
        if ser is None:
            return False
        _ = ser.in_waiting
        return True
    except Exception:  # noqa: BLE001
        return False


class DaemonTransport:
    """Transport implementation backed by a local daemon Unix socket."""

    def __init__(self, path: Path, timeout: float = 3.0) -> None:
        self.path = Path(path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(str(self.path))
        self.sock.setblocking(False)

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        self.sock.sendall(data.encode("utf-8", errors="replace"))

    def read(self, timeout: float = 0.05) -> str:
        readable, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        if not readable:
            return ""
        try:
            data = self.sock.recv(8192)
        except BlockingIOError:
            return ""
        if not data:
            raise RuntimeError("m4adb daemon disconnected")
        return data.decode("utf-8", errors="replace")

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


def daemon_alive(path: Path, timeout: float = 0.25) -> bool:
    try:
        t = DaemonTransport(path, timeout=timeout)
        t.close()
        return True
    except OSError:
        return False


def stop_daemon(path: Path, timeout: float = 1.0) -> bool:
    """Ask a daemon to stop; returns false when no daemon is listening."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(path))
        s.sendall(b"@M4ADBD/1 shutdown\n")
        s.close()
        return True
    except OSError:
        return False


class BridgeDaemon:
    """Single-owner serial forwarder with automatic serial reconnection.

    The Unix socket stays bound for the daemon's lifetime.  When the device
    disappears (USB unplug, reset after flash, CDC handle death), the daemon
    does NOT exit: it keeps accepting clients (they get a controlled error or
    timeout) and retries opening the serial port with bounded backoff until
    the device returns.
    """

    #: Serial reopen backoff bounds (seconds).
    RECONNECT_BASE_DELAY = 0.5
    RECONNECT_MAX_DELAY = 8.0

    def __init__(self, port: str, baud: int, path: Path) -> None:
        self.port = port
        self.baud = int(baud)
        self.path = Path(path)
        self.serial: Optional[SerialTransport] = None
        self.listener: Optional[socket.socket] = None
        self.peer: Optional[socket.socket] = None
        self.running = True
        self._reconnect_delay = self.RECONNECT_BASE_DELAY

    def _close_peer(self) -> None:
        if self.peer is not None:
            try:
                self.peer.close()
            except OSError:
                pass
            self.peer = None

    def _close_serial(self) -> None:
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:  # noqa: BLE001
                pass
            self.serial = None

    def _cleanup(self) -> None:
        self._close_peer()
        if self.listener is not None:
            try:
                self.listener.close()
            except OSError:
                pass
            self.listener = None
        self._close_serial()
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass

    def _open_serial(self, ready_timeout: float) -> bool:
        """Open (or reopen) the serial port and wait for the bridge to be
        ready.  Returns True only when the device is usable."""
        try:
            self._close_serial()
            self.serial = SerialTransport(self.port, self.baud)
            from .client import Client

            Client(self.serial, default_timeout=3).wait_ready(timeout=ready_timeout)
            self._reconnect_delay = self.RECONNECT_BASE_DELAY
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"m4adb daemon reconnect: {exc}", file=sys.stderr, flush=True)
            self._close_serial()
            return False

    def serve(self, ready_timeout: float = 60.0) -> int:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.exists():
                if daemon_alive(self.path):
                    print(f"daemon already running: {self.path}", file=sys.stderr, flush=True)
                    return 2
                # Stale socket: previous daemon died without unlinking.
                self.path.unlink()
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(str(self.path))
            os.chmod(self.path, 0o600)
            self.listener.listen(1)
            self.listener.settimeout(0.05)
            print(f"m4adb daemon socket={self.path}", file=sys.stderr, flush=True)
            # First serial open happens here, once per daemon lifetime.
            if not self._open_serial(ready_timeout):
                print("m4adb daemon: serial open failed at startup; retrying in background",
                      file=sys.stderr, flush=True)
            else:
                print(f"m4adb daemon ready socket={self.path} port={self.port}", flush=True)
            while self.running:
                # Accept CLI peers even while the serial is down so clients get
                # a clean error instead of a refused connection.
                if self.peer is None:
                    try:
                        self.peer, _ = self.listener.accept()
                        self.peer.setblocking(False)
                    except socket.timeout:
                        pass
                    except OSError:
                        break
                if self.peer is not None:
                    try:
                        readable, _, _ = select.select([self.peer], [], [], 0)
                        if readable:
                            data = self.peer.recv(8192)
                            if not data:
                                self._close_peer()
                            elif data == b"@M4ADBD/1 shutdown\n":
                                self.running = False
                            elif self.serial is not None:
                                self.serial.write(data.decode("utf-8", errors="replace"))
                    except (BlockingIOError, ConnectionError, OSError):
                        self._close_peer()
                # Pump device output continuously, even between CLI calls.
                if self.serial is not None:
                    try:
                        data = self.serial.read(0.01)
                        if data and self.peer is not None:
                            self.peer.sendall(data.encode("utf-8", errors="replace"))
                    except (ConnectionError, OSError) as exc:
                        # The e-ink main loop can block USB CDC briefly during a
                        # long refresh; a transient read error must NOT kill the
                        # daemon. Only give up the serial when the device is
                        # really gone, then schedule a reconnect.
                        if not serial_port_alive(self.serial):
                            print(f"m4adb daemon: device lost ({exc}); will reconnect",
                                  file=sys.stderr, flush=True)
                            self._close_peer()
                            self._close_serial()
                elif self.running:
                    # Device is gone; retry with bounded backoff.
                    try:
                        time.sleep(self._reconnect_delay)
                    except KeyboardInterrupt:
                        self.running = False
                        break
                    if self._open_serial(ready_timeout):
                        print(f"m4adb daemon reconnected port={self.port}", flush=True)
                    else:
                        self._reconnect_delay = min(
                            self._reconnect_delay * 2, self.RECONNECT_MAX_DELAY)
        except Exception as exc:  # pragma: no cover - exercised on real host
            print(f"m4adb daemon error: {exc}", file=sys.stderr, flush=True)
            return 1
        finally:
            self._cleanup()
        return 0
