#!/usr/bin/env python3
"""Murphy M4 simulated UI — free resize + letterbox scale + click-to-tap.

Window is freely resizable. The 480×800 panel always letterboxes into the
canvas (aspect locked). Toolbar / tap / key are prioritised over screenshots
so the UI stays responsive while Live is on.

Examples::

  python3 simulator/tools/m4_screen_viewer.py --pty /dev/ttys006
  python3 simulator/tools/m4_screen_viewer.py /tmp/m4-home.pbm
"""
from __future__ import annotations

import argparse
import queue
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable

import tkinter as tk
from tkinter import filedialog, messagebox

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_W, DEFAULT_H = 480, 800
BEZEL = 6
TOOLBAR_H = 36
STATUS_H = 24
MIN_WIN_W = 360
MIN_WIN_H = 360
RESIZE_DEBOUNCE_MS = 40
SWIPE_MIN_PX = 32
# Left-side physical-key strip — Murphy M4 side body keys (BoardConfig + NETS.md):
#   top→bottom: KEY1/Up(GPIO1), KEY2/Down(GPIO2), KEY_LOCK/Power(GPIO0), Recovery(4th)
# Back/Confirm/Left/Right are touch-mapped on M4, not these four GPIOs.
KEYPAD_W = 72
# (label, action) — action is m4adb key name, or special "home" for recovery soft path
SIDE_KEYS = (
    ("上\nUp", "Up"),
    ("下\nDown", "Down"),
    ("电源\nPwr", "Power"),
    ("恢复\nRec", "Recovery"),  # 4th physical; SW path: soft home (no RST GPIO in firmware)
)


# ---------------------------------------------------------------------------
# PBM helpers
# ---------------------------------------------------------------------------


def load_pbm_p4(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    if raw.startswith(b"P4"):
        i = 2
        while i < len(raw) and raw[i] in b" \t\r\n":
            i += 1
        while i < len(raw) and raw[i] == ord("#"):
            while i < len(raw) and raw[i] not in b"\n":
                i += 1
            while i < len(raw) and raw[i] in b" \t\r\n":
                i += 1

        def read_token() -> str:
            nonlocal i
            while i < len(raw) and raw[i] in b" \t\r\n":
                i += 1
            if i < len(raw) and raw[i] == ord("#"):
                while i < len(raw) and raw[i] not in b"\n":
                    i += 1
                return read_token()
            j = i
            while j < len(raw) and raw[j] not in b" \t\r\n":
                j += 1
            tok = raw[i:j].decode("ascii")
            i = j
            return tok

        w = int(read_token())
        h = int(read_token())
        while i < len(raw) and raw[i] in b" \t\r":
            i += 1
        if i < len(raw) and raw[i] == ord("\n"):
            i += 1
        payload = raw[i:]
        row_bytes = (w + 7) // 8
        expect = row_bytes * h
        if len(payload) < expect:
            raise ValueError(f"PBM truncated: need {expect} bytes, got {len(payload)}")
        return w, h, payload[:expect]

    if len(raw) == 48000:
        return 480, 800, raw
    raise ValueError(f"unsupported image: {path} size={len(raw)}")


def physical_to_logical(w: int, h: int, payload: bytes) -> tuple[int, int, bytes]:
    """Rotate the SSD1677 800×480 PBM frame into the logical 480×800 panel."""
    if (w, h) != (800, 480):
        return w, h, payload
    out = bytearray(480 * 800 // 8)
    for y in range(800):
        for x in range(480):
            src_x, src_y = y, 479 - x
            if (payload[src_y * 100 + src_x // 8] >> (7 - src_x % 8)) & 1:
                out[y * 60 + x // 8] |= 1 << (7 - x % 8)
    return 480, 800, bytes(out)


_BYTE_TO_GRAY = tuple(
    bytes(0 if value & (0x80 >> bit) else 255 for bit in range(8))
    for value in range(256)
)


def pbm_to_pgm_scaled(
    w: int,
    h: int,
    payload: bytes,
    dw: int,
    dh: int,
    *,
    invert: bool = False,
) -> bytes:
    """Nearest-neighbour 1bpp → grayscale PGM of size dw×dh."""
    dw = max(1, int(dw))
    dh = max(1, int(dh))
    row_bytes = (w + 7) // 8
    if (dw, dh) == (w, h) and w % 8 == 0:
        pixels = b"".join(_BYTE_TO_GRAY[b] for b in payload)
        if invert:
            pixels = bytes(255 - value for value in pixels)
        return f"P5\n{dw} {dh}\n255\n".encode("ascii") + pixels

    out = bytearray(dw * dh)
    xs = [min(w - 1, (x * w) // dw) for x in range(dw)]
    rows: dict[int, bytes] = {}
    for y in range(dh):
        sy = min(h - 1, (y * h) // dh)
        scaled = rows.get(sy)
        if scaled is None:
            row = payload[sy * row_bytes : (sy + 1) * row_bytes]
            scaled = bytes(
                (255 if invert else 0) if row[sx // 8] & (0x80 >> (sx % 8))
                else (0 if invert else 255)
                for sx in xs
            )
            rows[sy] = scaled
        out[y * dw : (y + 1) * dw] = scaled
    return f"P5\n{dw} {dh}\n255\n".encode("ascii") + bytes(out)


def letterbox(cw: int, ch: int) -> tuple[float, int, int, int, int]:
    """Fit 480×800 into cw×ch. → (scale, panel_w, panel_h, ox, oy)."""
    inner_w = max(1, int(cw) - BEZEL * 2)
    inner_h = max(1, int(ch) - BEZEL * 2)
    s = min(inner_w / DEFAULT_W, inner_h / DEFAULT_H)
    s = max(0.05, min(s, 4.0))
    pw = max(1, int(DEFAULT_W * s))
    ph = max(1, int(DEFAULT_H * s))
    # Snap scale to the integer panel so aspect stays exact
    s = min(pw / DEFAULT_W, ph / DEFAULT_H)
    pw = max(1, int(round(DEFAULT_W * s)))
    ph = max(1, int(round(DEFAULT_H * s)))
    ox = max(0, (int(cw) - pw) // 2)
    oy = max(0, (int(ch) - ph) // 2)
    return s, pw, ph, ox, oy


# ---------------------------------------------------------------------------
# m4adb worker — input has higher priority than screenshots
# ---------------------------------------------------------------------------


class _Job:
    __slots__ = ("kind", "args", "on_done", "prio", "seq")

    def __init__(
        self,
        kind: str,
        args: tuple[Any, ...] = (),
        on_done: Callable[[Any, Exception | None], None] | None = None,
        prio: int = 5,
        seq: int = 0,
    ) -> None:
        self.kind = kind
        self.args = args
        self.on_done = on_done
        self.prio = prio  # lower = sooner
        self.seq = seq

    def __lt__(self, other: "_Job") -> bool:
        if self.prio != other.prio:
            return self.prio < other.prio
        return self.seq < other.seq


class M4AdbWorker:
    def __init__(self, pty: str, *, baud: int = 115200, timeout: float = 12.0) -> None:
        self.pty = pty
        self.baud = baud
        self.timeout = timeout
        self._q: queue.PriorityQueue[_Job | None] = queue.PriorityQueue()
        self._client: Any = None
        self._thread = threading.Thread(target=self._run, name="m4adb-worker", daemon=True)
        self._ready = threading.Event()
        self._error: str | None = None
        self._lock = threading.Lock()
        self._shot_pending = False
        self._input_pending = 0
        self._closed = False
        self._seq = 0

    def start(self) -> None:
        self._thread.start()
        self.submit(_Job("connect", prio=0))

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        # Unblock queue
        self._q.put(_Job("quit", prio=99, seq=10**9))
        self._thread.join(timeout=3.0)

    def _next_seq(self) -> int:
        with self._lock:
            self._seq += 1
            return self._seq

    def submit(self, job: _Job) -> None:
        if self._closed:
            return
        if job.seq == 0:
            job.seq = self._next_seq()
        if job.kind == "screenshot":
            with self._lock:
                if self._shot_pending:
                    return
                self._shot_pending = True
        if job.kind in ("tap", "swipe", "key", "back", "home", "ui", "wifi_transfer"):
            with self._lock:
                if job.kind in ("tap", "swipe", "key") and self._input_pending >= 2:
                    if job.on_done is not None:
                        job.on_done(None, RuntimeError("input queue busy"))
                    return
                self._input_pending += 1
        self._q.put(job)

    def has_input_pending(self) -> bool:
        with self._lock:
            return self._input_pending > 0

    def tap(self, x: int, y: int, on_done: Callable | None = None) -> None:
        self.submit(_Job("tap", (x, y), on_done, prio=1))

    def swipe(self, sx: int, sy: int, ex: int, ey: int, on_done: Callable | None = None) -> None:
        self.submit(_Job("swipe", (sx, sy, ex, ey), on_done, prio=1))

    def key(self, name: str, on_done: Callable | None = None) -> None:
        self.submit(_Job("key", (name,), on_done, prio=1))

    def back(self, on_done: Callable | None = None) -> None:
        self.submit(_Job("back", (), on_done, prio=1))

    def home(self, on_done: Callable | None = None) -> None:
        # Highest priority among input: user expects immediate return to Home.
        self.submit(_Job("home", (), on_done, prio=0))

    def wifi_transfer(self, on_done: Callable | None = None) -> None:
        self.submit(_Job("wifi_transfer", (), on_done, prio=1))

    def screenshot(self, out: Path, on_done: Callable | None = None) -> None:
        # Screenshots are low priority so taps/keys stay snappy
        self.submit(_Job("screenshot", (out,), on_done, prio=8))

    def ui(self, on_done: Callable | None = None) -> None:
        self.submit(_Job("ui", (), on_done, prio=2))

    def _ensure_path(self) -> None:
        s = str(ROOT / "firmware" / "scripts")
        if s not in sys.path:
            sys.path.insert(0, s)

    def _open_client(self) -> Any:
        self._ensure_path()
        from m4adb_lib.client import Client  # type: ignore
        from m4adb_lib.daemon import DaemonTransport, daemon_alive, socket_path_for_port  # type: ignore
        from m4adb_lib.transport import SerialTransport  # type: ignore

        sock = socket_path_for_port(self.pty)
        if daemon_alive(sock):
            try:
                c = Client(DaemonTransport(sock, timeout=1.5), default_timeout=self.timeout)
                c.wait_ready(timeout=8.0)
                return c
            except Exception:
                try:
                    c.close()
                except Exception:
                    pass

        c = Client(SerialTransport(self.pty, self.baud), default_timeout=self.timeout)
        c.wait_ready(timeout=max(15.0, self.timeout))
        return c

    def _log(self, msg: str) -> None:
        try:
            with open("/tmp/m4_viewer_worker.log", "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")
        except Exception:
            pass

    def _run(self) -> None:
        self._log(f"worker start pty={self.pty}")
        while True:
            try:
                job = self._q.get(timeout=0.05)
            except queue.Empty:
                # Keep unsolicited firmware logs from filling the PTY. Requests
                # and draining share this worker, so response frames cannot race.
                if self._client is not None:
                    try:
                        self._client.t.read(timeout=0.01)
                    except Exception:
                        try:
                            self._client.close()
                        except Exception:
                            pass
                        self._client = None
                continue
            if job is None or job.kind == "quit":
                break
            result: Any = None
            err: Exception | None = None
            t0 = time.time()
            self._log(f"job {job.kind} prio={job.prio} begin")
            try:
                if job.kind == "connect":
                    if self._client is not None:
                        try:
                            self._client.close()
                        except Exception:
                            pass
                    self._client = self._open_client()
                    self._ready.set()
                    self._error = None
                    result = "ready"
                else:
                    if self._client is None:
                        self._client = self._open_client()
                        self._ready.set()
                    result = self._dispatch(job)
            except Exception as e:
                err = e
                self._error = str(e)
                self._log(f"job {job.kind} ERR {e!r}")
                try:
                    if self._client is not None:
                        self._client.close()
                except Exception:
                    pass
                self._client = None
            finally:
                ms = int((time.time() - t0) * 1000)
                self._log(f"job {job.kind} end {ms}ms err={err!r}")
                if job.kind == "screenshot":
                    with self._lock:
                        self._shot_pending = False
                if job.kind in ("tap", "swipe", "key", "back", "home", "ui", "wifi_transfer"):
                    with self._lock:
                        self._input_pending = max(0, self._input_pending - 1)
                if job.on_done is not None:
                    try:
                        job.on_done(result, err)
                    except Exception as cb_err:
                        self._log(f"on_done err {cb_err!r}")

        if self._client is not None:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None
        self._log("worker exit")

    def _dispatch(self, job: _Job) -> Any:
        c = self._client
        assert c is not None
        if job.kind == "tap":
            x, y = job.args
            return c.tap(int(x), int(y))
        if job.kind == "swipe":
            sx, sy, ex, ey = job.args
            return c.swipe(int(sx), int(sy), int(ex), int(ey))
        if job.kind == "key":
            (name,) = job.args
            return c.key(str(name))
        if job.kind == "back":
            return c.back()
        if job.kind == "home":
            return c.home()
        if job.kind == "wifi_transfer":
            return c.wifi_transfer(timeout=45)
        if job.kind == "ui":
            return c.ui()
        if job.kind == "screenshot":
            (out,) = job.args
            return c.screenshot(Path(out), timeout=45)
        raise ValueError(f"unknown job {job.kind}")


# ---------------------------------------------------------------------------
# Viewer
# ---------------------------------------------------------------------------


class M4ScreenViewer(tk.Tk):
    def __init__(
        self,
        path: Path | None,
        *,
        pty: str | None,
        frame_file: Path | None,
        interval: float,
        invert: bool,
        click_to_tap: bool,
        init_scale: float | None = None,
    ) -> None:
        super().__init__()
        self.title("Murphy M4 — Screen")
        self.configure(bg="#1a1a1a")
        self.minsize(MIN_WIN_W, MIN_WIN_H)
        self.resizable(True, True)

        self._path = path
        self._pty = pty
        self._frame_file = frame_file
        self._frame_signature: tuple[int, int] | None = None
        self._interval_ms = max(40 if frame_file else 500, int(interval * 1000))
        self._invert = invert
        self._click_to_tap = click_to_tap

        self._photo: tk.PhotoImage | None = None
        self._raw: tuple[int, int, bytes] | None = None  # logical frame
        self._img_w = DEFAULT_W
        self._img_h = DEFAULT_H
        self._frame_gen = 0
        self._paint_gen = 0
        self._frame_label = "—"

        # Canvas size last seen from Configure (authoritative for layout)
        self._cw = 1
        self._ch = 1
        self._disp_scale = 1.0
        self._panel_w = DEFAULT_W
        self._panel_h = DEFAULT_H
        self._ox = 0
        self._oy = 0
        self._layout_key: tuple[int, int, int, int] | None = None

        self._live = False
        self._live_after: str | None = None
        self._resize_after: str | None = None
        self._worker: M4AdbWorker | None = None
        self._press_logical: tuple[int, int] | None = None
        self._drag_item: int | None = None
        self._hit_items: list[int] = []

        # ----- toolbar -----
        toolbar = tk.Frame(self, bg="#111111", height=TOOLBAR_H)
        toolbar.pack(side=tk.TOP, fill=tk.X)
        toolbar.pack_propagate(False)

        def _btn(text: str, cmd) -> tk.Button:
            b = tk.Button(
                toolbar,
                text=text,
                command=cmd,
                bg="#2a2a2a",
                fg="#e0e0e0",
                activebackground="#4a4a4a",
                activeforeground="#ffffff",
                disabledforeground="#666666",
                relief=tk.RAISED,
                bd=1,
                padx=6,
                pady=2,
                font=("", 11),
                takefocus=0,
            )
            b.pack(side=tk.LEFT, padx=2, pady=4)
            return b

        _btn("Open", self.open_file)
        _btn("Reload", self.reload)
        self._live_btn = _btn("Live", self.toggle_live)
        _btn("Inv", self.toggle_invert)
        self._tap_btn = _btn(
            "Tap:On" if click_to_tap else "Tap:Off", self.toggle_click_tap
        )
        _btn("Home", lambda: self.send_nav("home"))
        _btn("传书", lambda: self.send_nav("wifi_transfer"))
        _btn("50%", lambda: self._set_window_scale(0.5))
        _btn("100%", lambda: self._set_window_scale(1.0))
        _btn("150%", lambda: self._set_window_scale(1.5))

        # ----- status -----
        status_bar = tk.Frame(self, bg="#111111", height=STATUS_H)
        status_bar.pack(side=tk.TOP, fill=tk.X)
        status_bar.pack_propagate(False)
        self._status = tk.StringVar(value="starting…")
        tk.Label(
            status_bar,
            textvariable=self._status,
            fg="#ccc",
            bg="#111111",
            anchor=tk.W,
            font=("", 11),
        ).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=8)

        # ----- body: left keypad (4 physical keys) + screen canvas -----
        body = tk.Frame(self, bg="#1a1a1a")
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        keypad = tk.Frame(body, bg="#151515", width=KEYPAD_W)
        keypad.pack(side=tk.LEFT, fill=tk.Y)
        keypad.pack_propagate(False)
        tk.Label(
            keypad,
            text="侧键",
            fg="#888",
            bg="#151515",
            font=("", 10),
        ).pack(side=tk.TOP, pady=(6, 2))

        self._key_btns: dict[str, tk.Button] = {}
        for label, key_name in SIDE_KEYS:
            b = tk.Button(
                keypad,
                text=label,
                command=lambda k=key_name: self.send_key(k),
                bg="#2c2c2c",
                fg="#f0f0f0",
                activebackground="#3d5a80",
                activeforeground="#ffffff",
                relief=tk.RAISED,
                bd=2,
                font=("", 11),
                justify=tk.CENTER,
                takefocus=0,
            )
            b.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=6, pady=4)
            self._key_btns[key_name] = b

        # Keyboard shortcuts match side keys top→bottom
        self.bind("<KeyPress-Up>", lambda e: self.send_key("Up"))
        self.bind("<KeyPress-Down>", lambda e: self.send_key("Down"))
        self.bind("<KeyPress-p>", lambda e: self.send_key("Power"))
        self.bind("<KeyPress-P>", lambda e: self.send_key("Power"))
        self.bind("<KeyPress-r>", lambda e: self.send_key("Recovery"))
        self.bind("<KeyPress-R>", lambda e: self.send_key("Recovery"))
        self.bind("<KeyPress-Home>", lambda e: self.send_key("Recovery"))

        self._canvas = tk.Canvas(
            body,
            bg="#0d0d0d",
            bd=0,
            highlightthickness=0,
            cursor="crosshair",
        )
        self._canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self._bezel_id = self._canvas.create_rectangle(
            0, 0, 10, 10, outline="#555", width=1, fill="#1c1c1c", tags=("bezel",)
        )
        self._panel_bg_id = self._canvas.create_rectangle(
            0, 0, 10, 10, outline="#888", width=1, fill="#666666", tags=("panel_bg",)
        )
        self._blank = tk.PhotoImage(width=1, height=1)
        self._img_item = self._canvas.create_image(
            0, 0, anchor=tk.NW, image=self._blank, tags=("screen",)
        )

        self._canvas.bind("<ButtonPress-1>", self.on_press)
        self._canvas.bind("<B1-Motion>", self.on_motion)
        self._canvas.bind("<ButtonRelease-1>", self.on_release)
        # Both canvas and root: some Tk builds only fire one of them on resize
        self._canvas.bind("<Configure>", self._on_canvas_configure)
        self.bind("<Configure>", self._on_root_configure)

        # Initial geometry
        self.update_idletasks()
        sw = int(self.winfo_screenwidth())
        sh = int(self.winfo_screenheight())
        if init_scale and init_scale > 0:
            s0 = init_scale
        else:
            usable_h = max(280, sh - 140 - TOOLBAR_H - STATUS_H)
            s0 = min(0.9, usable_h / DEFAULT_H, (sw - 48) / DEFAULT_W)
            s0 = max(0.35, s0)
        win_w = min(
            sw - 24,
            max(MIN_WIN_W, int(DEFAULT_W * s0) + BEZEL * 2 + KEYPAD_W + 20),
        )
        win_h = min(sh - 80, max(MIN_WIN_H, int(DEFAULT_H * s0) + BEZEL * 2 + TOOLBAR_H + STATUS_H + 12))
        try:
            self.geometry(f"{win_w}x{win_h}+16+36")
        except tk.TclError:
            self.geometry(f"{win_w}x{win_h}")

        if self._pty:
            self._worker = M4AdbWorker(self._pty)
            self._worker.start()
            self._status.set(f"connecting {self._pty}…")
            self.after(200, self._poll_worker_ready)

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        # Layout once widgets are mapped, then load the first frame.
        self.after(30, self._bootstrap_content, path)

    def _bootstrap_content(self, path: Path | None) -> None:
        self._force_layout()
        if self._frame_file is not None:
            self._live = True
            self._live_btn.configure(text="Live●", bg="#1a4a1a")
            self.after(40, self.live_tick)
        elif path is not None and path.is_file():
            self.show_path(path)
        elif self._pty:
            self._live = True
            self._live_btn.configure(text="Live●", bg="#1a4a1a")
            self.after(200, self.live_tick)
        else:
            self._status.set("Open a .pbm or pass --pty  ·  drag edge to scale")

    # ----- lifecycle -----

    def _on_close(self) -> None:
        self._live = False
        for attr in ("_live_after", "_resize_after"):
            aid = getattr(self, attr)
            if aid is not None:
                try:
                    self.after_cancel(aid)
                except Exception:
                    pass
                setattr(self, attr, None)
        if self._worker is not None:
            self._worker.close()
            self._worker = None
        self.destroy()

    def _poll_worker_ready(self) -> None:
        w = self._worker
        if w is None:
            return
        if w._ready.is_set():
            self._status.set(f"ready  {w.pty}  ·  resize window to scale panel")
            return
        if w._error:
            self._status.set(f"connect: {w._error[:100]}")
        self.after(250, self._poll_worker_ready)

    # ----- layout -----

    def _on_root_configure(self, event: tk.Event) -> None:
        # Only react to the toplevel itself
        if event.widget is not self:
            return
        # Canvas size lags one event; schedule read of actual canvas size
        self._schedule_layout()

    def _on_canvas_configure(self, event: tk.Event) -> None:
        if event.widget is not self._canvas:
            return
        if event.width < 4 or event.height < 4:
            return
        self._cw = int(event.width)
        self._ch = int(event.height)
        self._schedule_layout()

    def _schedule_layout(self) -> None:
        if self._resize_after is not None:
            try:
                self.after_cancel(self._resize_after)
            except Exception:
                pass
        self._resize_after = self.after(RESIZE_DEBOUNCE_MS, self._apply_layout)

    def _force_layout(self) -> None:
        self.update_idletasks()
        self._cw = max(1, int(self._canvas.winfo_width()))
        self._ch = max(1, int(self._canvas.winfo_height()))
        self._apply_layout()

    def _apply_layout(self) -> None:
        self._resize_after = None
        cw = max(1, int(self._canvas.winfo_width()))
        ch = max(1, int(self._canvas.winfo_height()))
        if cw < 4 or ch < 4:
            return
        self._cw, self._ch = cw, ch

        s, pw, ph, ox, oy = letterbox(cw, ch)
        key = (pw, ph, ox, oy)
        size_changed = (
            self._layout_key is None
            or self._layout_key[0] != pw
            or self._layout_key[1] != ph
        )
        self._layout_key = key
        self._disp_scale = s
        self._panel_w = pw
        self._panel_h = ph
        self._ox = ox
        self._oy = oy

        # Decorations always track layout
        self._canvas.coords(self._bezel_id, 1, 1, cw - 1, ch - 1)
        self._canvas.coords(self._panel_bg_id, ox - 1, oy - 1, ox + pw, oy + ph)
        self._canvas.coords(self._img_item, ox, oy)

        if size_changed:
            self._paint_frame(reason="resize")
        else:
            # Still update status with current scale
            self._status.set(
                f"{self._frame_label}  "
                f"{self._img_w}×{self._img_h}→{pw}×{ph}  "
                f"{s:.2f}×  canvas {cw}×{ch}"
            )

    def _set_window_scale(self, scale: float) -> None:
        pw = int(DEFAULT_W * scale)
        ph = int(DEFAULT_H * scale)
        win_w = max(MIN_WIN_W, pw + BEZEL * 2 + KEYPAD_W + 24)
        win_h = max(MIN_WIN_H, ph + BEZEL * 2 + TOOLBAR_H + STATUS_H + 16)
        sw = int(self.winfo_screenwidth())
        sh = int(self.winfo_screenheight())
        win_w = min(win_w, sw - 16)
        win_h = min(win_h, sh - 64)
        self._status.set(f"window → {win_w}×{win_h} (scale {scale:.2f})")
        self.geometry(f"{win_w}x{win_h}")
        # Layout after geometry settles
        self.after(80, self._force_layout)

    # ----- paint -----

    def _paint_frame(self, *, reason: str = "") -> None:
        """Rasterise self._raw into a PhotoImage of current panel size."""
        if self._raw is None:
            self._status.set(
                f"panel {self._panel_w}×{self._panel_h}  "
                f"{self._disp_scale:.2f}×  (no frame)  {reason}"
            )
            return

        w, h, payload = self._raw
        pw, ph = max(1, self._panel_w), max(1, self._panel_h)
        self._paint_gen += 1
        gen = self._paint_gen

        try:
            ppm = pbm_to_pgm_scaled(w, h, payload, pw, ph, invert=self._invert)
            # master=self is required on some Tk builds so the image is not GC'd
            # against a withdrawn default root and reports real width/height.
            photo = tk.PhotoImage(master=self, data=ppm)
        except Exception as e:
            self._status.set(f"paint error ({reason}): {e}")
            return

        if photo.width() < 1 or photo.height() < 1:
            self._status.set(f"paint empty photo ({reason}) ppm={len(ppm)}")
            return

        # Drop stale paints if a newer layout superseded us mid-flight
        if gen != self._paint_gen:
            return

        self._photo = photo  # keep reference alive
        # Re-bind image; also re-create item if Tk dropped the previous bitmap
        try:
            self._canvas.itemconfigure(self._img_item, image=photo)
        except tk.TclError:
            self._img_item = self._canvas.create_image(
                self._ox, self._oy, anchor=tk.NW, image=photo, tags=("screen",)
            )
        self._canvas.coords(self._img_item, self._ox, self._oy)
        self._canvas.tag_raise("screen")
        self._canvas.tag_raise("hit")

        tap = "tap" if (self._pty and self._click_to_tap) else "view"
        self._status.set(
            f"{self._frame_label}  {w}×{h}→{pw}×{ph}  "
            f"{self._disp_scale:.2f}×  [{tap}]  f{self._frame_gen}  {reason}"
        )
        self.title(f"Murphy M4 — {self._frame_label} @ {self._disp_scale:.2f}×")

    def _set_raw_frame(self, w: int, h: int, payload: bytes, label: str) -> None:
        self._raw = (w, h, payload)
        self._img_w, self._img_h = w, h
        self._frame_label = label
        self._frame_gen += 1
        # Ensure we have a layout before painting
        if self._cw < 8 or self._ch < 8:
            self._force_layout()
        else:
            self._paint_frame(reason="frame")

    def show_path(self, path: Path) -> None:
        try:
            w, h, payload = load_pbm_p4(path)
            self._path = path
            self._set_raw_frame(w, h, payload, path.name)
        except Exception as exc:
            self._status.set(f"error: {exc}")
            messagebox.showerror("Load failed", str(exc))

    # ----- coordinates -----

    def _event_to_logical(self, event: tk.Event) -> tuple[int, int] | None:
        sx = self._disp_scale if self._disp_scale > 1e-6 else 1.0
        lx = int((event.x - self._ox) / sx)
        ly = int((event.y - self._oy) / sx)
        if lx < 0 or ly < 0 or lx >= DEFAULT_W or ly >= DEFAULT_H:
            return None
        return max(0, min(self._img_w - 1, lx)), max(0, min(self._img_h - 1, ly))

    def _flash_hit(self, lx: int, ly: int) -> None:
        sx = self._disp_scale if self._disp_scale > 1e-6 else 1.0
        cx = int(self._ox + lx * sx + sx / 2)
        cy = int(self._oy + ly * sx + sx / 2)
        r0 = max(5, int(5 * max(sx, 0.4)))
        color = "#ff3333"
        items = [
            self._canvas.create_oval(
                cx - r0, cy - r0, cx + r0, cy + r0, outline=color, width=2, tags=("hit",)
            ),
            self._canvas.create_line(
                cx - r0 - 4, cy, cx + r0 + 4, cy, fill=color, width=1, tags=("hit",)
            ),
            self._canvas.create_line(
                cx, cy - r0 - 4, cx, cy + r0 + 4, fill=color, width=1, tags=("hit",)
            ),
        ]
        self._hit_items.extend(items)
        self.after(300, lambda: self._clear_hit(items))

    def _clear_hit(self, items: list[int]) -> None:
        for it in items:
            try:
                self._canvas.delete(it)
            except tk.TclError:
                pass
            if it in self._hit_items:
                self._hit_items.remove(it)

    def on_press(self, event: tk.Event) -> None:
        pt = self._event_to_logical(event)
        self._press_logical = pt
        if pt is not None:
            self._flash_hit(pt[0], pt[1])
            self._status.set(f"↓ ({pt[0]},{pt[1]})  scale={self._disp_scale:.2f}")

    def on_motion(self, event: tk.Event) -> None:
        start = self._press_logical
        end = self._event_to_logical(event)
        if start is None or end is None:
            return
        sx = self._disp_scale if self._disp_scale > 1e-6 else 1.0
        coords = (self._ox + start[0] * sx, self._oy + start[1] * sx,
                  self._ox + end[0] * sx, self._oy + end[1] * sx)
        if self._drag_item is None:
            self._drag_item = self._canvas.create_line(*coords, fill="#2f80ed", width=3,
                                                       arrow=tk.LAST, tags=("hit",))
        else:
            self._canvas.coords(self._drag_item, *coords)

    def on_release(self, event: tk.Event) -> None:
        if self._drag_item is not None:
            self._canvas.delete(self._drag_item)
            self._drag_item = None
        if not self._click_to_tap:
            self._status.set("Tap is Off — click toolbar Tap:On")
            self._press_logical = None
            return
        if self._worker is None:
            messagebox.showinfo("No PTY", "Need --pty for click-to-tap")
            self._press_logical = None
            return
        start = self._press_logical
        pt = self._event_to_logical(event) or start
        self._press_logical = None
        if pt is None:
            self._status.set("click outside panel")
            return
        x, y = pt
        if start is not None and max(abs(x - start[0]), abs(y - start[1])) >= SWIPE_MIN_PX:
            self._send_swipe(start[0], start[1], x, y)
            return
        t0 = time.time()
        self._status.set(f"tap ({x},{y})…")

        def done(result: Any, err: Exception | None) -> None:
            def ui() -> None:
                ms = int((time.time() - t0) * 1000)
                if err is not None:
                    self._status.set(f"tap ({x},{y}) FAIL {ms}ms: {err}")
                else:
                    self._status.set(f"tap ({x},{y}) ok {ms}ms")
                    self.after(300, self.grab_live_once)

            self.after(0, ui)

        self._worker.tap(x, y, on_done=done)

    def _send_swipe(self, sx: int, sy: int, ex: int, ey: int) -> None:
        if self._worker is None:
            return
        t0 = time.time()
        dx, dy = ex - sx, ey - sy
        direction = ("左" if dx < 0 else "右") if abs(dx) >= abs(dy) else ("上" if dy < 0 else "下")
        self._status.set(f"swipe {direction}…")

        def done(result: Any, err: Exception | None) -> None:
            def ui() -> None:
                ms = int((time.time() - t0) * 1000)
                self._status.set(f"swipe {direction} {'FAIL: ' + str(err) if err else 'ok'} {ms}ms")
                if self._frame_file is None:
                    self.after(250, self.grab_live_once)
            self.after(0, ui)

        self._worker.swipe(sx, sy, ex, ey, on_done=done)

    # ----- toolbar actions -----

    def toggle_invert(self) -> None:
        self._invert = not self._invert
        self._status.set(f"invert={'on' if self._invert else 'off'}")
        self._paint_frame(reason="invert")

    def toggle_click_tap(self) -> None:
        self._click_to_tap = not self._click_to_tap
        self._tap_btn.configure(text="Tap:On" if self._click_to_tap else "Tap:Off")
        self._status.set(f"Click→Tap {'ON' if self._click_to_tap else 'OFF'}")

    def open_file(self) -> None:
        p = filedialog.askopenfilename(
            title="Open M4 PBM",
            filetypes=[("PBM / raw", "*.pbm *.bin *.raw"), ("All", "*.*")],
        )
        if p:
            self.show_path(Path(p))

    def reload(self) -> None:
        self._status.set("reload…")
        if self._pty:
            self.grab_live_once()
        elif self._path:
            self.show_path(self._path)

    def toggle_live(self) -> None:
        if not self._pty:
            messagebox.showinfo("Live", "Start with --pty /dev/ttysNNN")
            return
        self._live = not self._live
        if self._live:
            self._live_btn.configure(text="Live●", bg="#1a4a1a")
            self._status.set("Live ON")
            self.live_tick()
        else:
            self._live_btn.configure(text="Live", bg="#2a2a2a")
            self._status.set("Live OFF")
            if self._live_after is not None:
                try:
                    self.after_cancel(self._live_after)
                except Exception:
                    pass
                self._live_after = None

    def send_key(self, key_name: str) -> None:
        """Inject Murphy M4 side physical keys: Up / Down / Power / Recovery.

        BoardConfig (MURPHY_M4 input pins): up=GPIO1, down=GPIO2, power=GPIO0
        (active-low). Recovery is the 4th body button; firmware has no BTN_RECOVERY
        so the viewer soft-maps it to m4adb home (safe UI recover).
        """
        if self._worker is None:
            messagebox.showinfo("No PTY", "Key inject needs --pty")
            return
        # Flash the matching keypad button for immediate feedback
        btn = self._key_btns.get(key_name)
        if btn is not None:
            prev = btn.cget("bg")
            btn.configure(bg="#4a7ab5")
            self.after(120, lambda b=btn, p=prev: b.configure(bg=p))

        t0 = time.time()
        self._status.set(f"key {key_name}…")

        def done(result: Any, err: Exception | None) -> None:
            def ui() -> None:
                ms = int((time.time() - t0) * 1000)
                if err is not None:
                    self._status.set(f"key {key_name} FAIL {ms}ms: {err}")
                else:
                    self._status.set(f"key {key_name} ok {ms}ms")
                    delay = 900 if key_name in ("Power", "Recovery") else 350
                    self.after(delay, self.grab_live_once)

            self.after(0, ui)

        if key_name == "Recovery":
            # No dedicated Recovery BTN_* in firmware InputPins; soft recover → home.
            self._worker.home(on_done=done)
        elif key_name == "Power":
            self._worker.key("Power", on_done=done)
        else:
            # Up / Down (side page keys)
            self._worker.key(key_name, on_done=done)

    def send_nav(self, kind: str, name: str = "") -> None:
        if self._worker is None:
            messagebox.showinfo("No PTY", "Key inject needs --pty")
            return
        label = name or kind
        t0 = time.time()
        self._status.set(f"{label}…")

        def done(result: Any, err: Exception | None) -> None:
            def ui() -> None:
                ms = int((time.time() - t0) * 1000)
                if err is not None:
                    self._status.set(f"{label} FAIL {ms}ms: {err}")
                    return
                # Confirm activity for home, then refresh screenshot after redraw.
                delay = 900 if kind in ("home", "back") else 350
                self._status.set(f"{label} ok {ms}ms")

                def after_ui(u: Any, uerr: Exception | None) -> None:
                    def ui2() -> None:
                        act = ""
                        if uerr is None and isinstance(u, dict):
                            act = str(u.get("activity") or "")
                        if act:
                            self._status.set(f"{label} ok {ms}ms → {act}")
                        self.after(delay, self.grab_live_once)

                    self.after(0, ui2)

                if kind == "home" and self._worker is not None:
                    self._worker.ui(on_done=after_ui)
                else:
                    self.after(delay, self.grab_live_once)

            self.after(0, ui)

        if kind == "back":
            self._worker.back(on_done=done)
        elif kind == "home":
            # Firmware op "home" calls onGoHome() (not a key named Home).
            self._worker.home(on_done=done)
        elif kind == "wifi_transfer":
            self._worker.wifi_transfer(on_done=done)
        else:
            self._worker.key(name, on_done=done)

    def grab_live_once(self) -> None:
        if self._frame_file is not None:
            try:
                st = self._frame_file.stat()
                signature = (st.st_mtime_ns, st.st_size)
                if signature != self._frame_signature:
                    raw = physical_to_logical(*load_pbm_p4(self._frame_file))
                    self._frame_signature = signature
                    self._path = self._frame_file
                    self._set_raw_frame(*raw, "EPD direct")
            except (OSError, ValueError):
                pass  # QEMU may be between truncate/write; retry next tick.
            if self._live:
                self._live_after = self.after(self._interval_ms, self.live_tick)
            return
        if self._worker is None or not self._pty:
            return
        # Don't pile screenshots on top of pending input
        if self._worker.has_input_pending():
            if self._live:
                self._live_after = self.after(200, self.live_tick)
            return
        out_path = Path(tempfile.gettempdir()) / "m4_viewer_live.pbm"

        def done(result: Any, err: Exception | None) -> None:
            raw: tuple[int, int, bytes] | None = None
            decode_err: Exception | None = err
            if err is None and out_path.is_file():
                try:
                    raw = load_pbm_p4(out_path)
                except Exception as e:
                    decode_err = e

            def ui() -> None:
                if decode_err is not None:
                    self._status.set(f"capture fail: {decode_err}")
                elif raw is not None:
                    w, h, payload = raw
                    self._path = out_path
                    self._set_raw_frame(w, h, payload, "live")
                if self._live:
                    self._live_after = self.after(self._interval_ms, self.live_tick)

            self.after(0, ui)

        self._worker.screenshot(out_path, on_done=done)

    def live_tick(self) -> None:
        if not self._live:
            return
        self.grab_live_once()


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", nargs="?", help="path to .pbm screenshot")
    p.add_argument("--pty", help="serial PTY for m4adb live screenshot + tap")
    p.add_argument("--frame-file", help="live SSD1677 PBM exported directly by QEMU")
    p.add_argument("--interval", type=float, default=None, help="live refresh seconds")
    p.add_argument(
        "--scale",
        default="fit",
        help="initial window scale hint: fit, 0.5, 1, 1.5, 2",
    )
    p.add_argument("--invert", action="store_true")
    p.add_argument("--no-click-tap", action="store_true")
    args = p.parse_args(argv)

    init_scale: float | None
    s = str(args.scale).strip().lower().replace("×", "").replace("x", "")
    if s in ("fit", "auto", "0", ""):
        init_scale = None
    else:
        init_scale = float(s)
        if init_scale <= 0:
            init_scale = None

    path = Path(args.image).expanduser() if args.image else None
    if path is None and not args.pty:
        for cand in (
            Path("/tmp/m4-home.pbm"),
            Path(tempfile.gettempdir()) / "m4_viewer_live.pbm",
            ROOT / "simulator" / "artifacts" / "m4-home.pbm",
        ):
            if cand.is_file():
                path = cand
                break

    pty = args.pty
    if not pty:
        pty_file = Path("/tmp/m4-plugin-debug/artifacts/pty.txt")
        if pty_file.is_file():
            pty = pty_file.read_text(encoding="utf-8").strip() or None

    frame_file = Path(args.frame_file).expanduser() if args.frame_file else None
    if frame_file is None:
        frame_hint = Path("/tmp/m4-plugin-debug/artifacts/frame-file.txt")
        if frame_hint.is_file():
            frame_file = Path(frame_hint.read_text(encoding="utf-8").strip())
    interval = args.interval if args.interval is not None else (0.08 if frame_file else 2.5)

    app = M4ScreenViewer(
        path,
        pty=pty,
        frame_file=frame_file,
        interval=interval,
        invert=args.invert,
        click_to_tap=not args.no_click_tap,
        init_scale=init_scale,
    )
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
