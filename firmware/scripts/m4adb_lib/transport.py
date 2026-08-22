"""Serial and mock transports for m4adb."""

from __future__ import annotations

import sys
import time
from abc import ABC, abstractmethod
from typing import Optional


class Transport(ABC):
    @abstractmethod
    def write(self, data: str) -> None: ...

    @abstractmethod
    def read(self, timeout: float = 0.05) -> str: ...

    @abstractmethod
    def close(self) -> None: ...


def is_pipe_port(port: str) -> bool:
    """QEMU pipe base ends with .pipe (FIFOs are <base>.in/.out)."""
    return port.endswith(".pipe") or port.endswith(".pipe.in") or port.endswith(".pipe.out")


def make_transport(port: str, baud: int = 115200):
    """Factory: SerialTransport for PTY/USB, PipeTransport for QEMU ``pipe:`` backends."""
    if is_pipe_port(port):
        # Normalize to base without .in/.out suffix
        base = port
        if base.endswith(".in"):
            base = base[:-3]
        elif base.endswith(".out"):
            base = base[:-4]
        return PipeTransport(base)
    return SerialTransport(port, baud)


def is_pty_port(port: str) -> bool:
    """QEMU exposes the guest UART as a host PTY (/dev/ttysNNN or /dev/pts/N)."""
    path = port.replace("\\", "/")
    name = path.rsplit("/", 1)[-1]
    parent = path.rsplit("/", 2)[-2] if "/" in path else ""
    return name.startswith("ttys") or parent == "pts"


class SerialTransport(Transport):
    def __init__(self, port: str, baud: int = 115200) -> None:
        try:
            import serial  # type: ignore
        except ImportError as e:
            raise RuntimeError(
                "需要 pyserial：pip install pyserial\n"
                "Need pyserial: pip install pyserial"
            ) from e
        # Do not toggle DTR/RTS in a way that resets ESP32-S3 unless requested.
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = baud
        self._ser.timeout = 0.05
        self._ser.write_timeout = 5
        self._ser.dsrdtr = False
        self._ser.rtscts = False
        # Set inactive levels before opening.  Setting these only after open
        # allows pyserial's default DTR/RTS assertion to reset ESP32-S3 first.
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()
        # tcdrain/flush on a PTY waits until the guest UART reads. External TTF
        # paints block poll() for several seconds, so flush() makes ./m4sim ui
        # tap/key look timed out. USB serial still drains.
        self._drain_on_write = not is_pty_port(port)

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        payload = data.encode("utf-8", errors="replace")
        # USB-Serial/JTAG has a 64-byte FIFO and Arduino's RX queue is 256
        # bytes.  A single pyserial write can otherwise arrive as one burst,
        # overflow RX, and silently lose a long chk frame.  Pace only long
        # frames; normal control/status requests remain one write.
        if len(payload) <= 192:
            self._ser.write(payload)
            if self._drain_on_write:
                self._ser.flush()
            return
        for off in range(0, len(payload), 64):
            self._ser.write(payload[off : off + 64])
            if self._drain_on_write:
                self._ser.flush()
            if off + 64 < len(payload):
                # The owner loop may be in an e-ink refresh for several
                # milliseconds; 25 ms keeps the 256-byte RX queue from
                # overflowing even while the display task is busy.
                time.sleep(0.025)

    def read(self, timeout: float = 0.05) -> str:
        end = time.time() + timeout
        chunks: list[bytes] = []
        while time.time() < end:
            n = self._ser.in_waiting
            if n:
                chunks.append(self._ser.read(n))
            else:
                time.sleep(0.01)
            if chunks and self._ser.in_waiting == 0:
                # small settle
                time.sleep(0.005)
                if self._ser.in_waiting == 0:
                    break
        if not chunks:
            return ""
        return b"".join(chunks).decode("utf-8", errors="replace")

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


class PipeTransport(Transport):
    """QEMU pipe transport for sandboxed environments where PTY is blocked.

    QEMU is started with ``-serial pipe:<base>`` where <base>.in and <base>.out
    are FIFOs.  This transport opens them with O_NONBLOCK and implements the
    same pacing semantics as SerialTransport.
    """

    def __init__(self, pipe_base: str) -> None:
        import errno
        import os

        self.base = pipe_base
        self._in_path = pipe_base + ".in"
        self._out_path = pipe_base + ".out"
        # Host writes to .in (guest reads), reads from .out (guest writes).
        # Open non-blocking; retry a few times while QEMU creates the FIFOs.
        for _ in range(20):
            try:
                self._fd_in = os.open(self._in_path, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError as e:
                if e.errno in (errno.ENXIO, errno.ENOENT):
                    time.sleep(0.1)
                    continue
                raise
        else:
            raise RuntimeError(f"pipe .in not ready: {self._in_path}")
        for _ in range(20):
            try:
                self._fd_out = os.open(self._out_path, os.O_RDONLY | os.O_NONBLOCK)
                break
            except OSError as e:
                if e.errno in (errno.ENOENT,):
                    time.sleep(0.1)
                    continue
                raise
        else:
            try:
                os.close(self._fd_in)
            except Exception:
                pass
            raise RuntimeError(f"pipe .out not ready: {self._out_path}")

    def _write_all(self, payload: bytes) -> None:
        import errno
        import os

        off = 0
        while off < len(payload):
            try:
                n = os.write(self._fd_in, payload[off:])
                if n:
                    off += n
                    continue
            except OSError as e:
                if e.errno != errno.EAGAIN:
                    raise
            time.sleep(0.01)

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        payload = data.encode("utf-8", errors="replace")
        # Pace long frames like SerialTransport to avoid RX overflow.
        if len(payload) <= 192:
            self._write_all(payload)
            return
        for off in range(0, len(payload), 64):
            self._write_all(payload[off : off + 64])
            if off + 64 < len(payload):
                time.sleep(0.025)

    def read(self, timeout: float = 0.05) -> str:
        import errno
        import os

        end = time.time() + timeout
        chunks: list[bytes] = []
        while time.time() < end:
            try:
                data = os.read(self._fd_out, 4096)
                if data:
                    chunks.append(data)
                    # small settle: check if more immediately available
                    time.sleep(0.005)
                    continue
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                    raise
            time.sleep(0.01)
            if chunks:
                # if we have data and no more pending, return
                try:
                    extra = os.read(self._fd_out, 4096)
                    if extra:
                        chunks.append(extra)
                        continue
                except OSError:
                    pass
                break
        if not chunks:
            return ""
        return b"".join(chunks).decode("utf-8", errors="replace")

    def close(self) -> None:
        import os

        for fd in (getattr(self, "_fd_in", None), getattr(self, "_fd_out", None)):
            if fd is not None:
                try:
                    os.close(fd)
                except Exception:
                    pass


class MockTransport(Transport):
    """In-memory transport wired to MockDevice."""

    def __init__(self, device: "object") -> None:
        self.device = device
        self._rx = ""

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        resp = self.device.handle_line(data.rstrip("\n"))
        if resp:
            if not resp.endswith("\n"):
                resp += "\n"
            self._rx += resp

    def read(self, timeout: float = 0.05) -> str:
        # Mock device may also emit async chunks after write.
        extra = getattr(self.device, "drain_async", lambda: "")()
        if extra:
            self._rx += extra if extra.endswith("\n") or "\n" in extra else extra + "\n"
        out = self._rx
        self._rx = ""
        return out

    def close(self) -> None:
        pass


def list_serial_devices() -> list[dict]:
    """List plausible ESP32-S3 CDC serial devices (macOS + Linux)."""
    out: list[dict] = []
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        # Fallback: scan /dev
        import glob

        for p in sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")):
            out.append({"device": p, "description": "serial", "hwid": ""})
        return out

    for p in list_ports.comports():
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        dev = p.device or ""
        plausible = any(
            x in desc or x in hwid or x in dev.lower()
            for x in (
                "usbmodem",
                "usbserial",
                "wch",
                "cp210",
                "ch340",
                "esp32",
                "silicon labs",
                "ttyacm",
                "usb jtag",
                "serial",
            )
        )
        # Prefer CDC ACM style on macOS
        if "bluetooth" in desc:
            continue
        if plausible or "usbmodem" in dev.lower() or "ttyACM" in dev:
            out.append(
                {
                    "device": dev,
                    "description": p.description or "",
                    "hwid": p.hwid or "",
                    "vid": getattr(p, "vid", None),
                    "pid": getattr(p, "pid", None),
                }
            )
    return out


def auto_port() -> Optional[str]:
    devs = list_serial_devices()
    if not devs:
        return None
    # Prefer cu.usbmodem on macOS
    for d in devs:
        if "usbmodem" in d["device"].lower():
            return d["device"]
    return devs[0]["device"]
