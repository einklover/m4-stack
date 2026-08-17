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
