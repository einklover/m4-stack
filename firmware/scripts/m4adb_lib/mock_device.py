"""Host-side mock device implementing the serial debug protocol.

Exercises real failure paths — not a success-only stub.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
from dataclasses import dataclass, field
from typing import Any, Optional

from .protocol import PREFIX, PROTOCOL_VERSION, MAX_RAW_CHUNK, encode_json_b64, encode_bytes_b64, parse_line


def _b64json(obj: dict) -> str:
    return encode_json_b64(obj)


@dataclass
class MockDevice:
    firmware: str = "test-mock-1.0"
    activity: str = "Home"
    active_app: str = ""
    free_heap: int = 200000
    min_free_heap: int = 150000
    free_psram: int = 4_000_000
    sd_ok: bool = True
    screen_w: int = 480
    screen_h: int = 800
    orientation: int = 0
    wifi_connected: bool = True
    wifi_status: int = 3  # WL_CONNECTED
    wifi_ssid: str = "MockLAN"
    wifi_ip: str = "192.0.2.1"
    wifi_rssi: int = -42
    # installed apps: id -> {version, versionCode, sha256?}
    installed: dict[str, dict[str, Any]] = field(default_factory=dict)
    # package storage by inbox name
    packages: dict[str, bytes] = field(default_factory=dict)
    # logical framebuffer (1 bit packed, all white initially)
    fb: bytes = field(default_factory=lambda: bytes(((480 + 7) // 8) * 800))
    # behavior knobs
    fail_next_install: Optional[str] = None
    reject_path_traversal: bool = True
    rate_limit_input: bool = True
    last_input_ms: float = 0
    pending_synth: bool = False
    idem: dict[str, str] = field(default_factory=dict)
    _upload: Optional[dict[str, Any]] = None
    _async: list[str] = field(default_factory=list)
    logs: list[str] = field(default_factory=list)
    clock_ms: float = 1000.0

    def tick(self, ms: float = 10) -> None:
        self.clock_ms += ms

    def set_pixel_black(self, x: int, y: int) -> None:
        row_b = (self.screen_w + 7) // 8
        ba = bytearray(self.fb)
        if x < 0 or y < 0 or x >= self.screen_w or y >= self.screen_h:
            return
        i = y * row_b + (x // 8)
        ba[i] |= 0x80 >> (x % 8)
        self.fb = bytes(ba)

    def drain_async(self) -> str:
        if not self._async:
            return ""
        out = "\n".join(self._async) + "\n"
        self._async.clear()
        return out

    def handle_line(self, line: str) -> str:
        self.logs.append(line)
        fr = parse_line(line)
        if not fr:
            return ""
        if fr.req_id in self.idem:
            return self.idem[fr.req_id]
        if fr.kind == "chk":
            return self._handle_chk(fr.req_id, fr.seq or 0, fr.total or 0, fr.raw or b"")
        if fr.kind != "req" or not fr.json:
            return self._err(fr.req_id, "bad_json", "JSON 解析失败")
        return self._handle_req(fr.req_id, fr.json)

    def _ok(self, req_id: str, obj: dict, cache: bool = False) -> str:
        line = f"{PREFIX} {req_id} ok {_b64json(obj)}"
        if cache:
            self.idem[req_id] = line
        return line

    def _err(self, req_id: str, key: str, msg: str, cache: bool = False) -> str:
        line = f"{PREFIX} {req_id} err {_b64json({'error': key, 'message': msg})}"
        if cache:
            self.idem[req_id] = line
        return line

    def _validate_name(self, name: str) -> Optional[str]:
        if not name or len(name) < 5 or len(name) > 64:
            return "bad_name"
        if "/" in name or "\\" in name or ".." in name:
            return "path_traversal"
        if not name.lower().endswith(".m4x"):
            return "bad_ext"
        if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
            return "bad_name"
        return None

    def _handle_req(self, req_id: str, j: dict) -> str:
        op = j.get("op")
        if op in ("ping", "status"):
            return self._ok(
                req_id,
                {
                    "op": op,
                    "protocol": PROTOCOL_VERSION,
                    "firmware": self.firmware,
                    "activity": self.activity,
                    "active_app": self.active_app,
                    "free_heap": self.free_heap,
                    "min_free_heap": self.min_free_heap,
                    "free_psram": self.free_psram,
                    "sd_ok": self.sd_ok,
                    "screen_w": self.screen_w,
                    "screen_h": self.screen_h,
                    "orientation": self.orientation,
                    "wifi_connected": self.wifi_connected,
                    "wifi_status": self.wifi_status,
                    "wifi_ssid": self.wifi_ssid if self.wifi_connected else "",
                    "wifi_ip": self.wifi_ip if self.wifi_connected else "",
                    "wifi_rssi": self.wifi_rssi if self.wifi_connected else -127,
                    "caps": ["install", "install_http", "wifi_status", "wifi_prepare", "wifi_transfer",
                             "launch", "tap", "swipe", "key", "screenshot", "logs", "m4b3_status",
                             "m4b3_panel"],
                },
            )
        if op == "m4b3_status":
            return self._ok(
                req_id,
                {
                    "op": "m4b3_status",
                    "listening": False,
                    "connected": False,
                    "hello": False,
                    "peer": "",
                    "bind": "",
                    "port": 48624,
                    "accepted_frame_id": -1,
                    "accepted_crc": 0,
                    "keys": 0,
                    "patches": 0,
                    "nacks": 0,
                    "hellos": 0,
                    "pings": 0,
                    "bytes_rx": 0,
                    "bytes_tx": 0,
                    "apply_errors": 0,
                    "reconnects": 0,
                    "last_nack": 255,
                    "rx_filled": 0,
                    "free_heap": self.free_heap,
                    "min_free_heap": self.min_free_heap,
                    "free_psram": self.free_psram,
                    "panel_owner": 0,
                    "panel_busy": False,
                    "panel_pending": False,
                    "present_req": 0,
                    "present_ok": 0,
                    "present_coal": 0,
                    "present_drop": 0,
                    "present_err": 0,
                    "map_err": 0,
                    "panel_src_id": -1,
                    "panel_src_crc": 0,
                    "panel_crc": 0,
                    "panel_err": 0,
                    "panel_age_ms": 0,
                    "panel_corners": [0, 0, 0, 0],
                },
            )
        if op == "m4b3_panel":
            return self._ok(
                req_id,
                {
                    "op": "m4b3_panel",
                    "owner": 0,
                    "busy": False,
                    "pending": False,
                    "req": 0,
                    "ok": 0,
                    "coal": 0,
                    "drop": 0,
                    "err": 0,
                    "map_err": 0,
                    "src_id": -1,
                    "src_crc": 0,
                    "panel_crc": 0,
                    "last_err": 0,
                    "age_ms": 0,
                    "accepted_id": -1,
                    "accepted_crc": 0,
                    "corners": [0, 0, 0, 0],
                },
            )
        if op in ("wifi_status", "wifi_prepare", "wifi_transfer"):
            ready = bool(self.wifi_connected and self.wifi_ip)
            return self._ok(
                req_id,
                {
                    "op": op,
                    "ready": ready,
                    "connected": bool(self.wifi_connected),
                    "status": self.wifi_status,
                    "ssid": self.wifi_ssid if self.wifi_connected else "",
                    "ip": self.wifi_ip if self.wifi_connected else "",
                    "rssi": self.wifi_rssi if self.wifi_connected else -127,
                    "url": f"http://{self.wifi_ip}/" if ready else "",
                },
                cache=op != "wifi_status",
            )
        if op == "install_http":
            # Device-as-client path: mock fetches host LAN URL (stdlib).
            name = j.get("name", "")
            size = int(j.get("size") or 0)
            sha = (j.get("sha256") or "").lower()
            url = j.get("url") or ""
            err = self._validate_name(name)
            if err:
                return self._err(req_id, err, "文件名/路径非法", cache=True)
            if size <= 0 or size > 4 * 1024 * 1024:
                return self._err(req_id, "size_invalid", "包大小非法或超过上限", cache=True)
            if not re.fullmatch(r"[0-9a-fA-F]{64}", sha or ""):
                return self._err(req_id, "sha_invalid", "SHA-256 格式无效", cache=True)
            if not isinstance(url, str) or not url.startswith("http://"):
                return self._err(req_id, "url_scheme", "安装 URL 非法", cache=True)
            try:
                import urllib.request

                with urllib.request.urlopen(url, timeout=10) as resp:
                    body = resp.read()
            except Exception:
                return self._err(req_id, "http_download", "Wi-Fi 下载失败", cache=True)
            if len(body) != size:
                return self._err(req_id, "size_mismatch", "下载大小与声明不一致", cache=True)
            digest = hashlib.sha256(body).hexdigest()
            if digest != sha:
                return self._err(req_id, "sha_mismatch", "SHA-256 校验失败", cache=True)
            # Reuse commit logic via synthetic upload state.
            self._upload = {
                "name": name,
                "size": size,
                "sha": sha,
                "buf": bytearray(body),
                "next_seq": 1,
                "declared_total": 1,
                "last_seq": 0,
                "last_ack": None,
            }
            out = self._commit(req_id)
            # Tag transport for callers that care.
            if '"op":"install"' in out or True:
                # _commit already encoded ok; re-parse is heavy — leave as-is.
                # Host only checks op/noop/id. Optional: inject transport via second reply unused.
                pass
            return out
        if op == "install_begin":
            name = j.get("name", "")
            size = int(j.get("size") or 0)
            sha = j.get("sha256", "")
            err = self._validate_name(name)
            if err:
                return self._err(req_id, err, "文件名/路径非法")
            if size <= 0 or size > 4 * 1024 * 1024:
                return self._err(req_id, "size_invalid", "包大小非法或超过上限")
            if not re.fullmatch(r"[0-9a-fA-F]{64}", sha or ""):
                return self._err(req_id, "sha_invalid", "SHA-256 格式无效")
            if self._upload:
                return self._err(req_id, "busy", "设备忙，请稍后重试")
            self._upload = {
                "name": name,
                "size": size,
                "sha": sha.lower(),
                "buf": bytearray(),
                "next_seq": 0,
                "declared_total": None,
                "last_seq": None,
                "last_ack": None,
            }
            return self._ok(req_id, {"op": "install_begin", "ready": True}, cache=True)
        if op == "install_commit":
            return self._commit(req_id)
        if op == "install_abort":
            self._upload = None
            return self._ok(req_id, {"op": "install_abort", "ok": True}, cache=True)
        if op == "launch":
            app_id = j.get("app_id", "")
            if not re.fullmatch(r"[a-z0-9]+(\.[a-z0-9_-]+)+", app_id or ""):
                return self._err(req_id, "invalid_id", "应用 ID 非法", cache=True)
            if app_id not in self.installed:
                return self._err(req_id, "not_found", "应用未安装", cache=True)
            self.active_app = app_id
            self.activity = "AppRuntime"
            return self._ok(req_id, {"op": "launch", "app_id": app_id}, cache=True)
        if op == "tap":
            x, y = int(j.get("x", -1)), int(j.get("y", -1))
            if x < 0 or y < 0 or x >= self.screen_w or y >= self.screen_h:
                return self._err(req_id, "tap_oob", "点击坐标越界")
            if self.rate_limit_input and self.pending_synth:
                return self._err(req_id, "busy", "输入忙，请稍后重试")
            self.pending_synth = True
            self.tick(40)
            self.pending_synth = False
            # Visual change heuristic for tests
            self.set_pixel_black(x % self.screen_w, y % self.screen_h)
            return self._ok(req_id, {"op": "tap", "x": x, "y": y}, cache=True)
        if op == "swipe":
            sx, sy = int(j.get("sx", -1)), int(j.get("sy", -1))
            ex, ey = int(j.get("ex", -1)), int(j.get("ey", -1))
            if min(sx, sy, ex, ey) < 0 or sx >= self.screen_w or ex >= self.screen_w \
                    or sy >= self.screen_h or ey >= self.screen_h or (sx == ex and sy == ey):
                return self._err(req_id, "swipe_oob", "滑动坐标越界或轨迹为空")
            return self._ok(req_id, {"op": "swipe", "sx": sx, "sy": sy,
                                     "ex": ex, "ey": ey}, cache=True)
        if op == "key":
            name = j.get("name", "")
            if name not in (
                "Back",
                "Confirm",
                "Left",
                "Right",
                "Up",
                "Down",
                "PageBack",
                "PageForward",
                "back",
                "confirm",
            ):
                return self._err(req_id, "bad_key", "不支持的按键名")
            return self._ok(req_id, {"op": "key", "name": name}, cache=True)
        if op == "back":
            return self._ok(req_id, {"op": "back"}, cache=True)
        if op == "home":
            self.activity = "Home"
            self.active_app = ""
            return self._ok(req_id, {"op": "home"}, cache=True)
        if op == "screenshot":
            return self._screenshot(req_id)
        return self._err(req_id, "unknown_op", "未知操作")

    def _handle_chk(self, req_id: str, seq: int, total: int, data: bytes) -> str:
        if not self._upload:
            return self._err(req_id, "no_upload", "没有进行中的上传")
        if total == 0 or seq >= total:
            self._upload = None
            return self._err(req_id, "bad_chunk_seq", "分片序号/总数非法")
        if self._upload["declared_total"] is None:
            self._upload["declared_total"] = total
        elif self._upload["declared_total"] != total:
            self._upload = None
            return self._err(req_id, "total_mismatch", "分片总数不一致")
        # Idempotent retry of same request id for last accepted chunk.
        if (
            self._upload.get("last_ack")
            and req_id in self.idem
            and self._upload.get("last_seq") == seq
        ):
            return self.idem[req_id]
        if seq != self._upload["next_seq"]:
            self._upload = None
            return self._err(req_id, "seq_mismatch", "分片序号不连续")
        if len(data) == 0 or len(data) > MAX_RAW_CHUNK:
            self._upload = None
            return self._err(req_id, "bad_chunk", "分片数据无效或过大")
        if len(self._upload["buf"]) + len(data) > self._upload["size"]:
            self._upload = None
            return self._err(req_id, "size_overflow", "分片超出声明大小")
        self._upload["buf"].extend(data)
        self._upload["next_seq"] += 1
        self._upload["last_seq"] = seq
        ack = {"chunk": seq, "received": len(self._upload["buf"])}
        self._upload["last_ack"] = ack
        return self._ok(req_id, ack, cache=True)

    def _commit(self, req_id: str) -> str:
        u = self._upload
        if not u:
            return self._err(req_id, "no_upload", "没有进行中的上传")
        if u["declared_total"] is None or u["next_seq"] != u["declared_total"]:
            self._upload = None
            return self._err(req_id, "incomplete_chunks", "上传未完成或不完整")
        if len(u["buf"]) != u["size"]:
            self._upload = None
            return self._err(req_id, "size_mismatch", "上传不完整")
        digest = hashlib.sha256(u["buf"]).hexdigest()
        if digest != u["sha"]:
            self._upload = None
            return self._err(req_id, "sha_mismatch", "SHA-256 校验失败", cache=True)
        if self.fail_next_install:
            key = self.fail_next_install
            self.fail_next_install = None
            self._upload = None
            return self._err(req_id, key, "安装失败", cache=True)
        import io
        import zipfile

        app_id = "unknown.app"
        version = "0.0.0"
        version_code = 1
        try:
            with zipfile.ZipFile(io.BytesIO(bytes(u["buf"]))) as zf:
                mf = json.loads(zf.read("manifest.json").decode("utf-8"))
                app_id = mf.get("id", app_id)
                version = mf.get("version", version)
                version_code = int(mf.get("versionCode", version_code))
        except Exception:
            pass
        prev = self.installed.get(app_id)
        if prev and int(prev.get("versionCode", 0)) > version_code:
            self._upload = None
            return self._err(req_id, "downgrade", "已安装更高版本，拒绝降级", cache=True)
        # No-op ONLY when package content SHA matches prior package of same name.
        existing = self.packages.get(u["name"])
        noop = bool(existing is not None and hashlib.sha256(existing).hexdigest() == digest)
        if not noop:
            self.packages[u["name"]] = bytes(u["buf"])
            self.installed[app_id] = {
                "version": version,
                "versionCode": version_code,
                "sha256": digest,
            }
        else:
            # still fill metadata from probe of existing content
            if prev:
                version = prev.get("version", version)
                version_code = int(prev.get("versionCode", version_code))
        self._upload = None
        return self._ok(
            req_id,
            {
                "op": "install",
                "noop": noop,
                "id": app_id,
                "version": version,
                "versionCode": version_code,
            },
            cache=True,
        )

    def _screenshot(self, req_id: str) -> str:
        raw = self.fb
        total = len(raw)
        meta = self._ok(
            req_id,
            {
                "op": "screenshot",
                "w": self.screen_w,
                "h": self.screen_h,
                "orientation": self.orientation,
                "bytes": total,
                "format": "pbm_raw",
            },
        )
        # Queue chunks + done as async lines
        chunks = [raw[i : i + MAX_RAW_CHUNK] for i in range(0, total, MAX_RAW_CHUNK)] or [b""]
        n = len(chunks)
        for i, c in enumerate(chunks):
            self._async.append(f"{PREFIX} {req_id} chk {i} {n} {encode_bytes_b64(c)}")
        sha = hashlib.sha256(raw).hexdigest()
        self._async.append(
            f"{PREFIX} {req_id} ok {_b64json({'op': 'screenshot_done', 'sha256': sha, 'bytes': total})}"
        )
        return meta
