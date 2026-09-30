"""Single-owner identity for m4adb (lock + pidfile + attach policy).

Exactly one process may open a given serial port. That process is the daemon.
CLI processes attach through the Unix socket. A ping timeout or unauthorized
bridge is a device-state error — never a reason to spawn a second owner.
"""

from __future__ import annotations

import fcntl
import hashlib
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Literal, Optional

AttachAction = Literal["reuse", "wait", "spawn", "refuse", "direct"]


def port_digest(port: str) -> str:
    return hashlib.sha1(port.encode("utf-8", errors="replace")).hexdigest()[:12]


@dataclass(frozen=True)
class OwnerPaths:
    socket: Path
    lock: Path
    pid: Path
    digest: str


def owner_paths(port: str, tmp: Optional[Path] = None) -> OwnerPaths:
    root = Path(tmp) if tmp is not None else Path("/tmp")
    digest = port_digest(port)
    return OwnerPaths(
        socket=root / f"m4adb-{digest}.sock",
        lock=root / f"m4adb-{digest}.lock",
        pid=root / f"m4adb-{digest}.pid",
        digest=digest,
    )


def socket_path_for_port(port: str, tmp: Optional[Path] = None) -> Path:
    """Stable per-port Unix socket path (does not leak the full device path)."""
    return owner_paths(port, tmp).socket


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def read_owner_pid(pid_path: Path) -> Optional[int]:
    try:
        raw = pid_path.read_text(encoding="utf-8").strip().splitlines()[0]
        pid = int(raw)
    except (OSError, IndexError, ValueError):
        return None
    return pid if pid_alive(pid) else None


def lock_is_held(lock_path: Path) -> bool:
    """True when another process holds LOCK_EX on the per-port lock file."""
    try:
        fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT, 0o600)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


def decide_attach(
    *,
    socket_alive: bool,
    lock_held: bool,
    no_daemon: bool,
) -> AttachAction:
    """How a CLI should attach to a port.

    reuse  — socket is accepting; talk through it (never spawn, never stop).
    wait   — lock is held but the socket is not accepting yet (daemon starting).
    spawn  — no owner; this CLI may start exactly one daemon.
    refuse — ``--no-daemon`` while an owner already exists (would dual-open).
    direct — ``--no-daemon`` and the port is free (QEMU / fault isolation).
    """
    if no_daemon:
        if socket_alive or lock_held:
            return "refuse"
        return "direct"
    if socket_alive:
        return "reuse"
    if lock_held:
        return "wait"
    return "spawn"


class ExclusivePortLock:
    """fcntl.flock exclusive lock. Held only by the daemon process."""

    def __init__(self, lock_path: Path, pid_path: Path) -> None:
        self.lock_path = Path(lock_path)
        self.pid_path = Path(pid_path)
        self._fd: Optional[int] = None

    @classmethod
    def for_port(cls, port: str, tmp: Optional[Path] = None) -> "ExclusivePortLock":
        paths = owner_paths(port, tmp)
        return cls(paths.lock, paths.pid)

    def held_by_self(self) -> bool:
        return self._fd is not None

    def try_acquire(self) -> bool:
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(self.lock_path), os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(fd)
            return False
        self._fd = fd
        os.lseek(fd, 0, os.SEEK_SET)
        os.ftruncate(fd, 0)
        os.write(fd, f"{os.getpid()}\n".encode("ascii"))
        os.fsync(fd)
        try:
            self.pid_path.write_text(f"{os.getpid()}\n", encoding="utf-8")
        except OSError:
            pass
        return True

    def release(self) -> None:
        fd = self._fd
        self._fd = None
        if fd is None:
            return
        try:
            self.pid_path.unlink()
        except FileNotFoundError:
            pass
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass
