"""High-level m4adb client over a Transport."""

from __future__ import annotations

import hashlib
import itertools
import json
import secrets
import time
from pathlib import Path
from typing import Any, Callable, Optional

from . import package as pkg
from .pbm import write_p4
from .protocol import (
    LineResyncParser,
    build_chk,
    build_req,
    chunk_bytes,
    UPLOAD_RAW_CHUNK,
)
from .transport import Transport


class BridgeError(Exception):
    def __init__(self, key: str, message: str = "") -> None:
        super().__init__(f"{key}: {message}")
        self.key = key
        self.message = message


class Client:
    def __init__(
        self,
        transport: Transport,
        default_timeout: float = 10.0,
        log_sink: Optional[Callable[[str], None]] = None,
    ) -> None:
        self.t = transport
        self.default_timeout = default_timeout
        self.log_sink = log_sink
        # The device caches idempotent replies across a persistent daemon
        # session.  A fresh CLI process must not restart at r1 or a new status
        # request could be mistaken for an older wifi/install request.
        self._session_tag = secrets.token_hex(3)
        self.parser = LineResyncParser()
        self._id = itertools.count(1)
        self.serial_log: list[str] = []

    def _next_id(self) -> str:
        return f"r{self._session_tag}{next(self._id)}"

    def _emit_log(self, line: str) -> None:
        self.serial_log.append(line)
        if self.log_sink:
            self.log_sink(line)

    def _read_until(
        self,
        pred: Callable[[Any], bool],
        timeout: float,
        collect_chunks: bool = False,
    ) -> Any:
        deadline = time.time() + timeout
        chunks: list[tuple[int, bytes]] = []
        last = None
        while time.time() < deadline:
            data = self.t.read(timeout=0.05)
            if not data:
                continue
            for raw_line, frame in self.parser.feed(data):
                if frame is None:
                    self._emit_log(raw_line)
                    continue
                self._emit_log(raw_line)
                if frame.kind == "chk" and collect_chunks:
                    chunks.append((frame.seq or 0, frame.raw or b""))
                    last = frame
                    continue
                if pred(frame):
                    if collect_chunks:
                        return frame, chunks
                    return frame
                last = frame
        raise BridgeError("timeout", f"等待响应超时 ({timeout}s)")

    def request(
        self,
        obj: dict[str, Any],
        timeout: Optional[float] = None,
        req_id: Optional[str] = None,
        on_progress: Optional[Callable[[dict[str, Any]], None]] = None,
        collect_chunks: bool = False,
    ) -> dict:
        """Send one request and wait for ok/err.

        on_progress: optional callback for intermediate ``prg`` frames (same req_id).
        Long ops (install_http) emit phase/pct so the host is not a silent wait.
        """
        rid = req_id or self._next_id()
        line = build_req(rid, obj)
        self.t.write(line)
        timeout = self.default_timeout if timeout is None else timeout
        deadline = time.time() + float(timeout)
        last_progress_at = time.time()
        last_heartbeat = time.time()
        chunks: dict[int, bytes] = {}

        while time.time() < deadline:
            data = self.t.read(timeout=0.1)
            if not data:
                # Host-side heartbeat every 5s while waiting (no device traffic).
                if on_progress and time.time() - last_heartbeat >= 5.0:
                    waited = time.time() - (deadline - float(timeout))
                    on_progress(
                        {
                            "op": obj.get("op"),
                            "phase": "host_wait",
                            "pct": -1,
                            "waited_s": round(waited, 1),
                            "remain_s": round(max(0.0, deadline - time.time()), 1),
                        }
                    )
                    last_heartbeat = time.time()
                continue
            for raw_line, frame in self.parser.feed(data):
                if frame is None:
                    self._emit_log(raw_line)
                    continue
                self._emit_log(raw_line)
                if frame.req_id != rid:
                    continue
                if frame.kind == "chk" and collect_chunks:
                    if frame.raw is not None:
                        chunks[frame.seq or 0] = frame.raw
                    continue
                if frame.kind == "prg":
                    last_progress_at = time.time()
                    last_heartbeat = time.time()
                    if on_progress:
                        on_progress(frame.json or {})
                    continue
                if frame.kind == "err":
                    j = frame.json or {}
                    raise BridgeError(j.get("error", "error"), j.get("message", ""))
                if frame.kind == "ok":
                    result = frame.json or {}
                    if collect_chunks and result.get("op") == "font_list_done":
                        try:
                            count = int(result["chunks"])
                            expected_bytes = int(result["bytes"])
                            if count < 0 or sorted(chunks) != list(range(count)):
                                raise ValueError("missing font list chunk")
                            raw = b"".join(chunks[index] for index in range(count))
                            if len(raw) != expected_bytes:
                                raise ValueError("font list byte count mismatch")
                            decoded = json.loads(raw.decode("utf-8"))
                        except (KeyError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError) as e:
                            raise BridgeError("font_list_invalid", str(e)) from e
                        if not isinstance(decoded, dict):
                            raise BridgeError("font_list_invalid", "字体列表响应不是 JSON 对象")
                        return decoded
                    return result
            # Stale: no prg for a long stretch still prints host_wait via heartbeat above.
            _ = last_progress_at
        raise BridgeError("timeout", f"等待响应超时 ({timeout}s)")

    def wait_ready(self, timeout: float = 15.0) -> dict:
        """Wait through an ESP32-S3 USB reconnect/reset until the bridge replies.

        Reuse one request id so a delayed reply and a retry are both safe.
        This must run on the same transport that will execute later commands.
        """
        deadline = time.time() + timeout
        last_error: Optional[BridgeError] = None
        while time.time() < deadline:
            try:
                return self.request(
                    {"op": "ping"},
                    timeout=min(1.0, max(0.05, deadline - time.time())),
                    req_id=f"ready_{self._session_tag}",
                )
            except BridgeError as e:
                if e.key != "timeout":
                    raise
                last_error = e
                time.sleep(0.2)
        raise last_error or BridgeError("timeout", f"等待设备就绪超时 ({timeout}s)")

    def ping(self) -> dict:
        return self.request({"op": "ping"}, timeout=5)

    def status(self) -> dict:
        return self.request({"op": "status"}, timeout=5)

    def font_list(self) -> dict:
        return self.request({"op": "font", "action": "list"}, timeout=15, collect_chunks=True)

    def font_get(self) -> dict:
        return self.request({"op": "font", "action": "get"}, timeout=5)

    def font_set(self, filename: str) -> dict:
        return self.request({"op": "font", "action": "set", "filename": filename}, timeout=45)

    def ui(self) -> dict:
        """Structured on-screen / plugin state (prefer over OCR for automation)."""
        return self.request({"op": "ui"}, timeout=8)

    def wifi_status(self) -> dict:
        """Read Wi-Fi/IP state without changing the radio or credentials."""
        return self.request({"op": "wifi_status"}, timeout=5)

    def wifi_prepare(self, timeout: float = 45.0) -> dict:
        """Use the persistent USB bridge to bring up saved STA Wi-Fi.

        The response contains only connection metadata (SSID/IP/RSSI); no
        password or credential material crosses the debug protocol.
        """
        return self.request({"op": "wifi_prepare"}, timeout=max(5.0, float(timeout)))

    def wifi_transfer(self, timeout: float = 45.0) -> dict:
        """Prepare Wi-Fi and open the native on-device file-transfer page."""
        return self.request({"op": "wifi_transfer"}, timeout=max(5.0, float(timeout)))

    def sd_probe(self) -> dict:
        """Run a bounded write/sync/read/delete probe on the device SD card."""
        return self.request({"op": "sd_probe"}, timeout=15)

    def http_probe(
        self,
        step: str,
        *,
        host: str = "weread.qq.com",
        url: str = "https://weread.qq.com/",
        session: bool = True,
        book_id: str = "",
        chapter_uid: str = "",
        psvts: str = "",
        app_id: str = "com.weread.client",
        wr_vid: str = "",
        wr_skey: str = "",
        wr_rt: str = "",
        timeout_ms: int = 30000,
        timeout: float = 90.0,
        on_progress: Optional[Callable[[dict[str, Any]], None]] = None,
    ) -> dict:
        """Run one isolated M4HttpTransport / WeRead fetch step on device.

        Steps: mem, debug_on, debug_off, session_begin, session_end, tls_get,
        weread_psvts, weread_e0, weread_t0, weread_t1, weread_e1, weread_e3,
        weread_set_cookie, shutdown.
        """
        return self.request(
            {
                "op": "http_probe",
                "step": step,
                "host": host,
                "url": url,
                "session": bool(session),
                "bookId": book_id,
                "chapterUid": chapter_uid,
                "psvts": psvts,
                "appId": app_id,
                "wr_vid": wr_vid,
                "wr_skey": wr_skey,
                "wr_rt": wr_rt,
                "timeout_ms": int(timeout_ms),
            },
            timeout=max(15.0, float(timeout)),
            on_progress=on_progress,
        )

    def sd_read(
        self,
        path: str,
        offset: int = -1,
        max_bytes: int = 400,
        timeout: float = 15.0,
    ) -> dict:
        """Read a bounded slice of an SD file over USB (apps_data / apps_inbox only).

        offset=-1 reads the tail (default).  Device caps max_bytes at 400.
        Response includes data_b64, size, offset, n, eof.
        """
        return self.request(
            {
                "op": "sd_read",
                "path": path,
                "offset": int(offset),
                "max": int(max_bytes),
            },
            timeout=max(5.0, float(timeout)),
        )

    def install(
        self,
        m4x_path: Path,
        inbox_name: Optional[str] = None,
        timeout: float = 120,
        chunk_retries: int = 3,
        chunk_timeout: float = 15.0,
        overall_timeout: Optional[float] = None,
        progress: Optional[Callable[[int, int, str], None]] = None,
        transport: str = "auto",
    ) -> dict:
        """Install a .m4x package.

        transport:
          - "wifi": host HTTP serve + device install_http (LAN Wi-Fi only)
          - "usb":  paced 512B logical chunks over 64B CDC slices (fallback)
          - "auto": try Wi-Fi first; fall back to USB on Wi-Fi/transport errors

        timeout: wall budget for device-side commit/install work.
        overall_timeout: hard wall for the whole transfer+install.
        """
        mode = (transport or "auto").lower().strip()
        if mode not in ("auto", "wifi", "usb"):
            raise BridgeError("bad_transport", f"未知 transport={transport!r}（auto|wifi|usb）")

        if mode == "usb":
            res = self.install_usb(
                m4x_path,
                inbox_name=inbox_name,
                timeout=timeout,
                chunk_retries=chunk_retries,
                chunk_timeout=chunk_timeout,
                overall_timeout=overall_timeout,
                progress=progress,
            )
            res = dict(res)
            res.setdefault("transport", "usb")
            return res

        try:
            res = self.install_wifi(
                m4x_path,
                inbox_name=inbox_name,
                timeout=timeout,
                overall_timeout=overall_timeout,
                progress=progress,
            )
            res = dict(res)
            res.setdefault("transport", "wifi")
            return res
        except BridgeError as e:
            if mode == "wifi":
                raise
            # auto only: fall back for old firmware / no Wi-Fi.  Keep
            # content and request validation errors terminal; all transport
            # and device-availability failures are safe to retry over USB.
            if e.key in {
                "sha_mismatch",
                "sha_invalid",
                "size_mismatch",
                "size_invalid",
                "bad_name",
                "bad_ext",
                "path_traversal",
            }:
                raise
            if progress:
                progress(0, 0, f"wifi_fallback:{e.key}")
            # If the device was still handling install_http when the control
            # request timed out, clear its staged transaction before starting
            # the USB upload on this same serial connection.
            try:
                self.request({"op": "install_abort"}, timeout=5)
            except BridgeError:
                pass
            res = self.install_usb(
                m4x_path,
                inbox_name=inbox_name,
                timeout=timeout,
                chunk_retries=chunk_retries,
                chunk_timeout=chunk_timeout,
                overall_timeout=overall_timeout,
                progress=progress,
            )
            res = dict(res)
            res.setdefault("transport", "usb")
            return res

    def install_wifi(
        self,
        m4x_path: Path,
        inbox_name: Optional[str] = None,
        timeout: float = 180,
        overall_timeout: Optional[float] = None,
        progress: Optional[Callable[[int, int, str], None]] = None,
    ) -> dict:
        """Serve .m4x on LAN HTTP; device downloads via Wi-Fi (one serial control frame)."""
        import urllib.error
        import urllib.request

        from .wifi_serve import WifiFileServer, detect_lan_ipv4

        m4x_path = Path(m4x_path)
        data = m4x_path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        # Stable short inbox name (policy-safe); avoids long hash filenames in URL.
        # Preserve the package filename by default; hard-coding weread.m4x
        # makes Fanqie and other plugins collide in /apps_inbox over Wi-Fi.
        name = inbox_name or m4x_path.name
        if not name.lower().endswith(".m4x"):
            name = name + ".m4x"
        err = pkg.validate_inbox_filename(name)
        if err:
            raise BridgeError(err, "主机侧文件名校验失败")
        wall = float(overall_timeout) if overall_timeout is not None else (float(timeout) + 60.0)
        deadline = time.time() + max(30.0, wall)

        lan = detect_lan_ipv4()
        if lan == "127.0.0.1" or lan.startswith("198.18."):
            raise BridgeError(
                "lan_ip",
                f"未找到可用局域网 IPv4（当前 {lan}）。请关闭 VPN/TUN 或确认 Wi-Fi IP。",
            )

        def _host_prg(phase: str, pct: int = 0, extra: str = "") -> None:
            if not progress:
                return
            # Reuse (done, total, phase) channel; total=100 for pct style.
            label = phase if not extra else f"{phase}:{extra}"
            progress(max(0, pct), 100, label)

        _host_prg("wifi_serve", 0)
        server = WifiFileServer(m4x_path)
        # Advertise real LAN IP (server binds 0.0.0.0).
        server.lan_ip = lan
        try:
            url = server.start()
            _host_prg("wifi_selfcheck", 2, url)
            # Host self-check: if we cannot fetch, device cannot either.
            try:
                with urllib.request.urlopen(url, timeout=3) as resp:
                    body = resp.read()
                if len(body) != len(data) or hashlib.sha256(body).hexdigest() != sha:
                    raise BridgeError("wifi_serve", "本机 HTTP 自检内容与包不一致")
            except BridgeError:
                raise
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                raise BridgeError(
                    "wifi_serve",
                    f"本机 HTTP 自检失败（{e}）。检查防火墙是否拦截入站端口 {server.port}。",
                ) from e
            _host_prg("wifi_url", 5, url)
            # USB is the control plane: explicitly wake/connect the saved STA
            # before asking the device to pull the host URL.  Older firmware
            # may not know wifi_prepare; install_http still performs its own
            # bounded connect path, so preserve compatibility in that case.
            try:
                prep = self.wifi_prepare(timeout=min(45.0, max(10.0, deadline - time.time())))
                if not prep.get("ready", prep.get("connected", False)):
                    raise BridgeError(
                        "wifi_required",
                        "设备未获得可用 Wi-Fi 地址；请在设备网络设置保存网络后重试",
                    )
                device_ip = str(prep.get("ip") or "")
                if device_ip:
                    _host_prg("wifi_device", 6, device_ip)
            except BridgeError as e:
                if e.key != "unknown_op":
                    raise
                # Compatibility with pre-wifi_prepare firmware.
                try:
                    self.status()
                except BridgeError:
                    pass
            remain = max(15.0, deadline - time.time())
            # Device download wall is ~30s + install; keep host wait a bit higher.
            remain = min(remain, max(60.0, float(timeout)))

            def on_device_prg(j: dict) -> None:
                if not progress:
                    return
                phase = str(j.get("phase") or "?")
                pct = j.get("pct")
                if phase == "host_wait":
                    progress(
                        0,
                        100,
                        f"host_wait:waited={j.get('waited_s')}s remain={j.get('remain_s')}s",
                    )
                    return
                try:
                    ipct = int(pct) if pct is not None and int(pct) >= 0 else 0
                except (TypeError, ValueError):
                    ipct = 0
                got = j.get("bytes")
                total = j.get("total")
                extra = ""
                if got is not None and total:
                    extra = f"{got}/{total}"
                progress(ipct, 100, f"device:{phase}" + (f":{extra}" if extra else ""))

            _host_prg("wifi_send", 8)
            res = self.request(
                {
                    "op": "install_http",
                    "name": name,
                    "size": len(data),
                    "sha256": sha,
                    "url": url,
                },
                timeout=remain,
                on_progress=on_device_prg,
            )
            _host_prg("wifi_done", 100)
            return res
        finally:
            server.close()

    def install_usb(
        self,
        m4x_path: Path,
        inbox_name: Optional[str] = None,
        timeout: float = 120,
        chunk_retries: int = 3,
        chunk_timeout: float = 15.0,
        overall_timeout: Optional[float] = None,
        progress: Optional[Callable[[int, int, str], None]] = None,
    ) -> dict:
        """Legacy USB CDC chunked upload + install_commit."""
        m4x_path = Path(m4x_path)
        data = m4x_path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        name = inbox_name or m4x_path.name
        err = pkg.validate_inbox_filename(name)
        if err:
            raise BridgeError(err, "主机侧文件名校验失败")
        wall = float(overall_timeout) if overall_timeout is not None else (float(timeout) + 90.0)
        deadline = time.time() + max(30.0, wall)

        def _remain() -> float:
            return max(0.05, deadline - time.time())

        def _check_overall(phase: str) -> None:
            if time.time() >= deadline:
                raise BridgeError(
                    "timeout",
                    f"安装整体超时 ({wall:.0f}s) 于 {phase}；"
                    "请确认唯一串口 owner、设备已开 USB 串口调试，勿并行打开监视器",
                )

        rid = self._next_id()
        if progress:
            progress(0, 0, "begin")
        _check_overall("install_begin")
        self.request(
            {"op": "install_begin", "name": name, "size": len(data), "sha256": sha},
            timeout=min(10.0, _remain()),
            req_id=rid,
        )
        parts = chunk_bytes(data, UPLOAD_RAW_CHUNK)
        total = len(parts)
        for seq, part in enumerate(parts):
            _check_overall(f"chunk {seq + 1}/{total}")
            # Stable request ID per chunk so lost-ack retries are idempotent.
            cid = f"{rid}c{seq}"
            last_err: Optional[BridgeError] = None
            for attempt in range(max(1, chunk_retries)):
                try:
                    self.t.write(build_chk(cid, seq, total, part))
                    fr = self._read_until(
                        lambda f, i=cid: f.req_id == i and f.kind in ("ok", "err"),
                        timeout=min(float(chunk_timeout), _remain()),
                    )
                    if fr.kind == "err":
                        j = fr.json or {}
                        raise BridgeError(j.get("error", "error"), j.get("message", ""))
                    last_err = None
                    break
                except BridgeError as e:
                    last_err = e
                    if e.key != "timeout" or attempt + 1 >= chunk_retries:
                        raise BridgeError(e.key, f"分片 {seq + 1}/{total}: {e.message}") from e
            if last_err:
                raise last_err
            if progress and (seq == 0 or seq + 1 == total or (seq + 1) % 32 == 0):
                progress(seq + 1, total, "upload")
        _check_overall("install_commit")
        if progress:
            progress(total, total, "commit")
        commit_to = min(float(timeout), _remain())
        return self.request({"op": "install_commit"}, timeout=max(5.0, commit_to))

    def send_raw_chunk(
        self,
        seq: int,
        total: int,
        part: bytes,
        timeout: float = 15.0,
        chunk_retries: int = 3,
    ) -> dict:
        """Send one raw chunk with a stable per-chunk request id (lab frames etc.)."""
        cid = f"labc{seq}"
        last_err: Optional[BridgeError] = None
        for attempt in range(max(1, chunk_retries)):
            try:
                self.t.write(build_chk(cid, seq, total, part))
                fr = self._read_until(
                    lambda f, i=cid: f.req_id == i and f.kind in ("ok", "err"),
                    timeout=timeout,
                )
                if fr.kind == "err":
                    j = fr.json or {}
                    raise BridgeError(j.get("error", "error"), j.get("message", ""))
                return fr.json or {}
            except BridgeError as e:
                last_err = e
                if e.key != "timeout" or attempt + 1 >= chunk_retries:
                    raise
        raise last_err  # pragma: no cover

    def launch(self, app_id: str) -> dict:        return self.request({"op": "launch", "app_id": app_id}, timeout=15)

    def tap(self, x: int, y: int) -> dict:
        # External TTF first-paint on QEMU commonly blocks poll() for ~5s+.
        return self.request({"op": "tap", "x": int(x), "y": int(y)}, timeout=20)

    def swipe(self, sx: int, sy: int, ex: int, ey: int) -> dict:
        return self.request({"op": "swipe", "sx": int(sx), "sy": int(sy),
                             "ex": int(ex), "ey": int(ey)}, timeout=20)

    def key(self, name: str) -> dict:
        return self.request({"op": "key", "name": name}, timeout=20)

    def back(self) -> dict:
        return self.request({"op": "back"}, timeout=5)

    def home(self) -> dict:
        return self.request({"op": "home"}, timeout=10)

    def screenshot(self, out: Path, timeout: float = 30) -> dict:
        rid = self._next_id()
        self.t.write(build_req(rid, {"op": "screenshot"}))
        meta_holder: dict[str, Any] = {}
        chunks: dict[int, bytes] = {}
        done = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self.t.read(0.05)
            if not data:
                continue
            for raw_line, frame in self.parser.feed(data):
                if frame is None:
                    self._emit_log(raw_line)
                    continue
                self._emit_log(raw_line)
                if frame.req_id != rid:
                    continue
                if frame.kind == "err":
                    j = frame.json or {}
                    raise BridgeError(j.get("error", "error"), j.get("message", ""))
                if frame.kind == "chk" and frame.raw is not None:
                    chunks[frame.seq or 0] = frame.raw
                    continue
                if frame.kind == "ok" and frame.json:
                    op = frame.json.get("op")
                    if op == "screenshot":
                        meta_holder = frame.json
                    elif op == "screenshot_done":
                        done = frame.json
            if meta_holder and done:
                break
        if not meta_holder or not done:
            raise BridgeError("timeout", "截图超时")
        expect = int(meta_holder.get("bytes") or 0)
        advertised_total = None
        if chunks:
            # A dropped CDC line must be reported as a missing sequence, not
            # silently sorted into a shorter bitmap and misdiagnosed later.
            first = min(chunks)
            last = max(chunks)
            if first != 0 or any(i not in chunks for i in range(first, last + 1)):
                missing = next(i for i in range(first, last + 1) if i not in chunks)
                raise BridgeError("frame_missing", f"截图串口帧缺失 seq={missing}")
            advertised_total = last + 1
        if advertised_total is not None and done.get("chunks") is not None:
            try:
                if int(done["chunks"]) != advertised_total:
                    raise BridgeError("frame_count", "截图帧总数不一致")
            except (TypeError, ValueError):
                pass
        ordered = b"".join(chunks[i] for i in sorted(chunks))
        if expect and len(ordered) != expect:
            raise BridgeError("size_mismatch", f"截图字节数不匹配 {len(ordered)}!={expect}")
        sha = hashlib.sha256(ordered).hexdigest()
        if done.get("sha256") and done["sha256"] != sha:
            raise BridgeError("sha_mismatch", "截图校验失败")
        w = int(meta_holder["w"])
        h = int(meta_holder["h"])
        write_p4(Path(out), w, h, ordered)
        return {**meta_holder, **done, "path": str(out), "sha256_local": sha}

    def close(self) -> None:
        self.t.close()
