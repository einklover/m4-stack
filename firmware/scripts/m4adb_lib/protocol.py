"""Versioned ASCII line framing for the M4 serial debug bridge.

Frame:
  @M4DBG/1 <req_id> <kind> [payload...]

kind:
  req  — host request, payload = base64(json)
  ok   — device success, payload = base64(json)
  err  — device error, payload = base64(json)
  chk  — binary chunk: <seq> <total> <base64>
"""

from __future__ import annotations

import base64
import json
import re
from dataclasses import dataclass
from typing import Any, Optional

PREFIX = "@M4DBG/1"
PROTOCOL_VERSION = 1
MAX_LINE_LEN = 1600
# Protocol receive ceiling: device screenshots are emitted in 768-byte chunks.
MAX_RAW_CHUNK = 768
# Host→device uploads use a 512-byte payload.  SerialTransport.write streams
# long frames in USB-sized slices with pacing so the ESP32-S3 hardware CDC
# queue (256 bytes) can drain while the line is assembled.  This avoids the
# old one-request-per-128-byte bottleneck without overrunning RX.
UPLOAD_RAW_CHUNK = 512
MAX_B64_CHUNK = 1400

# Unique prefix must not appear in ordinary logs as a full frame start.
FRAME_RE = re.compile(
    r"^@M4DBG/1\s+(\S+)\s+(req|ok|err|chk|prg)(?:\s+(.*))?$"
)


@dataclass
class Frame:
    req_id: str
    kind: str
    payload: str = ""
    seq: Optional[int] = None
    total: Optional[int] = None
    raw: Optional[bytes] = None
    json: Optional[dict[str, Any]] = None


def encode_json_b64(obj: dict[str, Any]) -> str:
    raw = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return base64.b64encode(raw).decode("ascii")


def decode_json_b64(payload: str) -> dict[str, Any]:
    # Strict: reject non-alphabet noise (validate=True).
    raw = base64.b64decode(payload.encode("ascii"), validate=True)
    if len(raw) > 8192:
        raise ValueError("json payload too large")
    return json.loads(raw.decode("utf-8"))


def encode_bytes_b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def decode_bytes_b64(payload: str) -> bytes:
    return base64.b64decode(payload.encode("ascii"), validate=True)


def build_req(req_id: str, obj: dict[str, Any]) -> str:
    return f"{PREFIX} {req_id} req {encode_json_b64(obj)}"


def build_chk(req_id: str, seq: int, total: int, data: bytes) -> str:
    if len(data) > MAX_RAW_CHUNK:
        raise ValueError("chunk too large")
    return f"{PREFIX} {req_id} chk {seq} {total} {encode_bytes_b64(data)}"


def parse_line(line: str) -> Optional[Frame]:
    """Parse one line. Returns None if not a bridge frame (ordinary log)."""
    line = line.rstrip("\r\n")
    if not line.startswith(PREFIX):
        return None
    if len(line) > MAX_LINE_LEN * 2:
        return None
    m = FRAME_RE.match(line)
    if not m:
        return None
    req_id, kind, rest = m.group(1), m.group(2), m.group(3) or ""
    if len(req_id) > 24:
        return None
    frame = Frame(req_id=req_id, kind=kind, payload=rest)
    if kind == "chk":
        parts = rest.split(" ", 2)
        if len(parts) < 3:
            return None
        try:
            frame.seq = int(parts[0])
            frame.total = int(parts[1])
        except ValueError:
            return None
        try:
            frame.raw = decode_bytes_b64(parts[2])
        except Exception:
            return None
        if len(frame.raw) > MAX_RAW_CHUNK + 64:
            return None
        return frame
    if kind in ("req", "ok", "err", "prg") and rest:
        try:
            frame.json = decode_json_b64(rest)
        except Exception:
            frame.json = None
    return frame


class LineResyncParser:
    """Byte/line parser that resynchronizes after garbage, partial data, or logs."""

    def __init__(self, max_line: int = MAX_LINE_LEN) -> None:
        self.max_line = max_line
        self._buf = ""

    def feed(self, data: str) -> list[tuple[str, Optional[Frame]]]:
        """Return list of (raw_line, frame_or_None)."""
        out: list[tuple[str, Optional[Frame]]] = []
        self._buf += data
        while True:
            nl = self._buf.find("\n")
            if nl < 0:
                if len(self._buf) > self.max_line:
                    # Drop until we can resync on prefix or clear overflow.
                    idx = self._buf.find(PREFIX)
                    if idx > 0:
                        self._buf = self._buf[idx:]
                    elif idx < 0 and len(self._buf) > self.max_line:
                        self._buf = self._buf[-len(PREFIX) :]
                break
            line = self._buf[:nl].rstrip("\r")
            self._buf = self._buf[nl + 1 :]
            if len(line) > self.max_line:
                out.append((line[:80] + "...", None))
                continue
            out.append((line, parse_line(line)))
        return out


def chunk_bytes(data: bytes, size: int = MAX_RAW_CHUNK) -> list[bytes]:
    return [data[i : i + size] for i in range(0, len(data), size)] or [b""]
