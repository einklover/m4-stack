"""Ephemeral LAN HTTP server for m4adb Wi-Fi package install.

Device is the HTTP *client*; this host process only serves one file once
(or until closed). No credentials, plain HTTP on a private bind address.
"""

from __future__ import annotations

import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional
from urllib.parse import quote


def _is_usable_lan_ipv4(ip: str) -> bool:
    """True for RFC1918 / loopback; false for VPN fake nets (e.g. 198.18/15)."""
    if not ip or ip.startswith("127."):
        return False
    parts = ip.split(".")
    if len(parts) != 4:
        return False
    try:
        a, b, c, d = (int(x) for x in parts)
    except ValueError:
        return False
    if any(x < 0 or x > 255 for x in (a, b, c, d)):
        return False
    # Clash/Surge TUN and similar often bind 198.18.0.0/15 — device cannot route there.
    if a == 198 and 18 <= b <= 19:
        return False
    if a == 10:
        return True
    if a == 192 and b == 168:
        return True
    if a == 172 and 16 <= b <= 31:
        return True
    return False


def detect_lan_ipv4() -> str:
    """Best-effort primary *real* LAN IPv4 (macOS/Linux).

    Prefer RFC1918 addresses from interfaces / UDP connect, skip VPN tunnel
    fakes such as 198.18.0.0/15 so the e-reader can actually HTTP-GET the host.
    """
    candidates: list[str] = []

    # 1) ifconfig / ip-style via getaddrinfo on hostname
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            cand = info[4][0]
            if _is_usable_lan_ipv4(cand):
                candidates.append(cand)
    except OSError:
        pass

    # 2) Parse ifconfig / ip (no extra deps; skips VPN fakes via filter)
    try:
        import re
        import subprocess

        for cmd in (("ifconfig",), ("ip", "-4", "addr")):
            try:
                out = subprocess.check_output(cmd, text=True, errors="replace", timeout=2)
            except (OSError, subprocess.SubprocessError):
                continue
            for cand in re.findall(r"inet(?:\s+addr:)?\s*(\d+\.\d+\.\d+\.\d+)", out):
                if _is_usable_lan_ipv4(cand):
                    candidates.append(cand)
            if candidates:
                break
    except Exception:
        pass

    # 3) UDP connect heuristic (may return VPN IP — filtered by _is_usable)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        cand = s.getsockname()[0]
        if _is_usable_lan_ipv4(cand):
            candidates.append(cand)
    except OSError:
        pass
    finally:
        s.close()

    # Prefer 192.168, then 10, then 172.16/12
    def rank(ip: str) -> tuple:
        a, b, *_ = (int(x) for x in ip.split("."))
        if a == 192 and b == 168:
            return (0, ip)
        if a == 10:
            return (1, ip)
        return (2, ip)

    # de-dupe preserve order
    seen = set()
    uniq = []
    for c in candidates:
        if c not in seen:
            seen.add(c)
            uniq.append(c)
    if uniq:
        uniq.sort(key=rank)
        return uniq[0]
    return "127.0.0.1"


class _OneFileHandler(BaseHTTPRequestHandler):
    # Set by server: Path, content type
    server: "OneFileServer"  # type: ignore[assignment]

    def log_message(self, fmt: str, *args) -> None:  # quiet by default
        pass

    def do_GET(self) -> None:  # noqa: N802
        want = self.server.url_path
        if self.path.split("?", 1)[0] != want:
            self.send_error(404, "not found")
            return
        data = self.server.file_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self) -> None:  # noqa: N802
        want = self.server.url_path
        if self.path.split("?", 1)[0] != want:
            self.send_error(404, "not found")
            return
        size = self.server.file_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Connection", "close")
        self.end_headers()


class OneFileServer(ThreadingHTTPServer):
    def __init__(self, bind: str, port: int, file_path: Path, url_path: str) -> None:
        super().__init__((bind, port), _OneFileHandler)
        self.file_path = file_path
        self.url_path = url_path
        self.daemon_threads = True


class WifiFileServer:
    """Serve one local file on 0.0.0.0:port until close()."""

    def __init__(self, file_path: Path, bind: str = "0.0.0.0", port: int = 0) -> None:
        self.file_path = Path(file_path).resolve()
        if not self.file_path.is_file():
            raise FileNotFoundError(self.file_path)
        # URL path uses the basename only (device policy rejects "..").
        name = self.file_path.name
        self.url_path = "/" + quote(name, safe="._-")
        self._httpd = OneFileServer(bind, port, self.file_path, self.url_path)
        host, bound_port = self._httpd.server_address[:2]
        self.bind_host = host
        self.port = int(bound_port)
        self.lan_ip = detect_lan_ipv4()
        self._thread: Optional[threading.Thread] = None

    @property
    def public_url(self) -> str:
        return f"http://{self.lan_ip}:{self.port}{self.url_path}"

    def start(self) -> str:
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="m4adb-wifi-serve", daemon=True)
        self._thread.start()
        return self.public_url

    def close(self) -> None:
        try:
            self._httpd.shutdown()
        except Exception:
            pass
        try:
            self._httpd.server_close()
        except Exception:
            pass
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None
