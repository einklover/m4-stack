"""Persistent USB-Serial/JTAG bridge for m4adb.

The ESP32-S3 can reset when a CDC handle is opened repeatedly.  This module
keeps exactly one transport open and exposes a 0600 Unix socket; CLI
invocations use the socket and never reopen the hardware port.

Guarantees:
  * Single instance per port: ``fcntl.flock`` on ``/tmp/m4adb-<digest>.lock``.
    A second ``serve()`` exits immediately (code 2) without touching serial.
  * No handshake in the daemon: it does not ping / wait_ready.  Clients do.
  * No reconnect-on-timeout: a quiet or unauthorized device is not "dead".
    Serial is reopened only after the port node vanished and reappeared,
    with timestamp backoff (the accept loop is never blocked on sleep).
  * Many CLI peers: listen backlog 32, line-buffered TX, RX broadcast.
"""

from __future__ import annotations

import os
import select
import socket
import sys
import time
from pathlib import Path
from typing import Callable, Optional

from .owner import ExclusivePortLock, owner_paths, socket_path_for_port
from .transport import make_transport, port_node_present

# Re-export: waveform_lab / screen viewer import these from daemon.
__all__ = [
    "BridgeDaemon",
    "DaemonTransport",
    "daemon_alive",
    "socket_path_for_port",
    "stop_daemon",
    "serial_port_alive",
    "should_reopen_serial",
]


def serial_port_alive(transport) -> bool:
    """Probe whether the underlying serial/pipe handle still responds."""
    fn = getattr(transport, "alive", None)
    if callable(fn):
        try:
            return bool(fn())
        except Exception:  # noqa: BLE001
            return False
    try:
        ser = getattr(transport, "_ser", None)
        if ser is not None:
            _ = ser.in_waiting
            return True
        if getattr(transport, "_fd_in", None) is not None:
            return True
        # Custom/test transports without pyserial internals stay open until
        # write/read fails or they implement alive().
        return True
    except Exception:  # noqa: BLE001
        return False


def should_reopen_serial(
    *,
    serial_open: bool,
    port_present: bool,
    last_attempt: float,
    now: float,
    delay: float,
) -> bool:
    """Reopen only when we have no handle, the node exists, and backoff elapsed.

    Ping timeout is intentionally not an input.  A live handle is never
    replaced just because the firmware is quiet or unauthorized.
    """
    if serial_open:
        return False
    if not port_present:
        return False
    if last_attempt > 0.0 and (now - last_attempt) < delay:
        return False
    return True


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
    """Ask a daemon to stop; returns false when no daemon is listening.

    Only ``m4adb daemon_stop`` should call this.  A ping timeout must not.
    """
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
    """Single-owner serial forwarder with multiplexed CLI peers.

    The Unix socket stays bound for the daemon's lifetime.  Serial is opened
    once and kept.  Reopen happens only after the port node disappears and
    comes back — never because a client handshake timed out.
    """

    RECONNECT_BASE_DELAY = 2.0
    RECONNECT_MAX_DELAY = 30.0
    LISTEN_BACKLOG = 32
    PEER_BUF_MAX = 65536

    def __init__(
        self,
        port: str,
        baud: int,
        path: Path,
        transport_factory: Optional[Callable[[], object]] = None,
        lock: Optional[ExclusivePortLock] = None,
    ) -> None:
        self.port = port
        self.baud = int(baud)
        self.path = Path(path)
        self._transport_factory = transport_factory
        self._lock = lock
        self.serial = None
        self.listener: Optional[socket.socket] = None
        self.peers: list[socket.socket] = []
        self._peer_buf: dict[int, bytes] = {}
        self.running = True
        self._reconnect_delay = 0.0  # first open is immediate
        self._last_open_attempt = 0.0
        self._owns_socket = False

    def _factory(self):
        if self._transport_factory is not None:
            return self._transport_factory()
        return make_transport(self.port, self.baud)

    def _close_peer(self, peer: socket.socket) -> None:
        try:
            peer.close()
        except OSError:
            pass
        if peer in self.peers:
            self.peers.remove(peer)
        self._peer_buf.pop(id(peer), None)

    def _close_all_peers(self) -> None:
        for peer in list(self.peers):
            self._close_peer(peer)

    def _close_serial(self) -> None:
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:  # noqa: BLE001
                pass
            self.serial = None

    def _cleanup(self) -> None:
        self._close_all_peers()
        if self.listener is not None:
            try:
                self.listener.close()
            except OSError:
                pass
            self.listener = None
        if self._owns_socket:
            try:
                self.path.unlink()
            except FileNotFoundError:
                pass
            self._owns_socket = False
        self._close_serial()
        if self._lock is not None:
            self._lock.release()
            self._lock = None

    def _try_open_serial(self) -> bool:
        """Open serial once.  Does not ping, does not close a live handle."""
        self._last_open_attempt = time.time()
        try:
            self.serial = self._factory()
            self._reconnect_delay = self.RECONNECT_BASE_DELAY
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"m4adb daemon serial open failed: {exc}", file=sys.stderr, flush=True)
            self._close_serial()
            if self._reconnect_delay <= 0:
                self._reconnect_delay = self.RECONNECT_BASE_DELAY
            else:
                self._reconnect_delay = min(
                    self._reconnect_delay * 2, self.RECONNECT_MAX_DELAY
                )
            return False

    def _broadcast(self, data: str) -> None:
        if not data:
            return
        raw = data.encode("utf-8", errors="replace")
        for peer in list(self.peers):
            try:
                peer.sendall(raw)
            except OSError:
                self._close_peer(peer)

    def _handle_peer_bytes(self, peer: socket.socket, data: bytes) -> None:
        key = id(peer)
        buf = self._peer_buf.get(key, b"") + data
        if len(buf) > self.PEER_BUF_MAX:
            buf = b""
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line_nl = line + b"\n"
            if line_nl == b"@M4ADBD/1 shutdown\n":
                self.running = False
                break
            if self.serial is not None:
                try:
                    self.serial.write(line_nl.decode("utf-8", errors="replace"))
                except Exception as exc:  # noqa: BLE001
                    print(f"m4adb daemon serial write: {exc}", file=sys.stderr, flush=True)
                    if not serial_port_alive(self.serial):
                        self._close_serial()
                        self._last_open_attempt = time.time()
        self._peer_buf[key] = buf

    def _pump_serial(self) -> None:
        if self.serial is None:
            return
        if not serial_port_alive(self.serial):
            present = port_node_present(self.port)
            print(
                f"m4adb daemon: serial handle dead (port_present={present}); "
                "will not reopen until backoff elapses",
                file=sys.stderr,
                flush=True,
            )
            self._close_serial()
            self._last_open_attempt = time.time()
            if self._reconnect_delay <= 0:
                self._reconnect_delay = self.RECONNECT_BASE_DELAY
            return
        try:
            data = self.serial.read(0.01)
            if data:
                self._broadcast(data)
        except (ConnectionError, OSError) as exc:
            present = port_node_present(self.port)
            print(
                f"m4adb daemon: serial read error ({exc}); port_present={present}",
                file=sys.stderr,
                flush=True,
            )
            if not present or not serial_port_alive(self.serial):
                self._close_serial()
                self._last_open_attempt = time.time()

    def _maybe_reopen(self) -> None:
        if not should_reopen_serial(
            serial_open=self.serial is not None,
            port_present=port_node_present(self.port),
            last_attempt=self._last_open_attempt,
            now=time.time(),
            delay=self._reconnect_delay,
        ):
            return
        if self._try_open_serial():
            print(f"m4adb daemon serial ready port={self.port}", flush=True)

    def serve(self, ready_timeout: float = 60.0) -> int:
        # ready_timeout is accepted for CLI compatibility and ignored: the
        # daemon never wait_ready()'s.  Handshake belongs to the CLI.
        _ = ready_timeout
        lock = self._lock
        if lock is None:
            paths = owner_paths(self.port)
            lock = ExclusivePortLock(paths.lock, paths.pid)
            self._lock = lock
        if not lock.try_acquire():
            print(
                f"m4adb daemon: port already owned (lock {lock.lock_path})",
                file=sys.stderr,
                flush=True,
            )
            return 2
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.exists():
                if daemon_alive(self.path):
                    # Live socket while we hold the flock: another owner without
                    # flock (legacy). Do not unlink their socket.
                    print(f"daemon already running: {self.path}", file=sys.stderr, flush=True)
                    return 2
                try:
                    self.path.unlink()
                except FileNotFoundError:
                    pass
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(str(self.path))
            self._owns_socket = True
            os.chmod(self.path, 0o600)
            self.listener.listen(self.LISTEN_BACKLOG)
            self.listener.setblocking(False)
            print(f"m4adb daemon socket={self.path}", file=sys.stderr, flush=True)
            if self._try_open_serial():
                print(f"m4adb daemon ready socket={self.path} port={self.port}", flush=True)
            else:
                print(
                    "m4adb daemon: serial not open yet; serving socket, will retry with backoff",
                    file=sys.stderr,
                    flush=True,
                )
            while self.running:
                rlist = [self.listener] + self.peers
                try:
                    readable, _, _ = select.select(rlist, [], [], 0.05)
                except (InterruptedError, ValueError):
                    readable = []
                if self.listener in readable:
                    try:
                        while True:
                            peer, _ = self.listener.accept()
                            peer.setblocking(False)
                            self.peers.append(peer)
                            self._peer_buf[id(peer)] = b""
                    except BlockingIOError:
                        pass
                    except OSError:
                        break
                for peer in list(self.peers):
                    if peer not in readable:
                        continue
                    try:
                        data = peer.recv(8192)
                    except BlockingIOError:
                        continue
                    except OSError:
                        self._close_peer(peer)
                        continue
                    if not data:
                        self._close_peer(peer)
                        continue
                    self._handle_peer_bytes(peer, data)
                self._pump_serial()
                self._maybe_reopen()
        except Exception as exc:  # pragma: no cover - exercised on real host
            print(f"m4adb daemon error: {exc}", file=sys.stderr, flush=True)
            return 1
        finally:
            self._cleanup()
        return 0
