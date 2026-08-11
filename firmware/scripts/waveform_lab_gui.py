#!/usr/bin/env python3
"""M4 Waveform Lab GUI — interactive slider control for page-turn tuning.

Safety: voltage tail (VGH/VSH/VSL/VCOM) stays LOCKED to the known-safe
factory values unless you explicitly check "unlock voltages".  All other
parameters are waveform timing / step counts — safe to sweep.

Controls:
  Steps        : number of strip steps in the wipe animation (2..32)
  TP (hex)     : waveform phase time unit for STEP LUT (0x01..0x30)
  Feather      : edge transition width in px (full-frame mode only)
  Settle phases: extra directional drives in the SETTLE LUT (1..4)
  Direction    : forward (page2 enters right->left) or backward

Buttons:
  Run animation     : baseline old -> strip wipe (STEP LUT)
  Settle            : full-frame old->new differential (SETTLE LUT)
  Run + Settle      : both, back to back
  Full clear        : safe recovery (FULL refresh to old page)
  Reset device      : re-open the m4adb daemon connection

All device traffic goes through the single m4adb daemon socket.
"""
from __future__ import annotations

import base64
import json
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from m4adb_lib.client import Client  # noqa: E402
from m4adb_lib.daemon import DaemonTransport, socket_path_for_port  # noqa: E402

LUT_BYTES = 110
SAFE_VOLTAGE_TAIL = bytes([0x17, 0x41, 0xA8, 0x32, 0x30])

# Default frame paths on the device SD.
DEFAULT_PREV = "/waveform/real_p1.bin"
DEFAULT_NEXT = "/waveform/real_p2b.bin"


def build_step_lut(tp: int) -> bytes:
    """Single-phase binary differential LUT (PAGE_STEP)."""
    lut = bytearray(LUT_BYTES)
    lut[10] = 0x80          # 01: black->white drive
    lut[20] = 0x40          # 10: white->black drive
    lut[50] = tp & 0xFF     # TP group0
    lut[51] = 0x01          # RP group0
    lut[80:85] = bytes([0x22] * 5)      # frame rate
    lut[105:110] = SAFE_VOLTAGE_TAIL
    return bytes(lut)


def build_settle_lut(phases: int, tp: int) -> bytes:
    """Multi-phase SETTLE LUT: `phases` same-direction drives for 01/10."""
    lut = bytearray(LUT_BYTES)
    phases = max(1, min(4, phases))
    for p in range(phases):
        lut[10 + p] = 0x80  # 01 phase p
        lut[20 + p] = 0x40  # 10 phase p
        g = p * 5
        lut[50 + g] = tp & 0xFF
        lut[50 + g + 1] = 0x01
    lut[80:85] = bytes([0x22] * 5)
    lut[105:110] = SAFE_VOLTAGE_TAIL
    return bytes(lut)


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


class M4AnimState:
    """Parse lut_stats to decide whether the device animation is running."""

    @staticmethod
    def is_running(st: dict) -> bool:
        return bool(st.get("animating"))


class WaveformLabGui:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("M4 波形实验台")
        root.geometry("620x760")

        self.client: Client | None = None
        self.lock = threading.Lock()

        self.prev_path = tk.StringVar(value=DEFAULT_PREV)
        self.next_path = tk.StringVar(value=DEFAULT_NEXT)
        self.steps = tk.IntVar(value=8)
        self.tp = tk.IntVar(value=0x08)
        self.feather = tk.IntVar(value=0)
        self.settle_phases = tk.IntVar(value=2)
        self.direction = tk.StringVar(value="forward")
        self.status = tk.StringVar(value="未连接")

        self._build_ui()
        self._connect()

    # ---------- UI ----------
    def _build_ui(self):
        pad = {"padx": 10, "pady": 4}

        frm = ttk.LabelFrame(self.root, text="帧（设备 SD 路径）")
        frm.pack(fill="x", **pad)
        ttk.Label(frm, text="旧页 (prev):").grid(row=0, column=0, sticky="w")
        ttk.Entry(frm, textvariable=self.prev_path, width=42).grid(row=0, column=1)
        ttk.Label(frm, text="新页 (next):").grid(row=1, column=0, sticky="w")
        ttk.Entry(frm, textvariable=self.next_path, width=42).grid(row=1, column=1)

        frm = ttk.LabelFrame(self.root, text="参数")
        frm.pack(fill="x", **pad)

        ttk.Label(frm, text="步数").grid(row=0, column=0, sticky="w")
        self.s_steps = ttk.Scale(frm, from_=2, to=32, variable=self.steps,
                                 command=lambda _: self._lbl_steps())
        self.s_steps.grid(row=0, column=1, sticky="ew")
        self.lbl_steps = ttk.Label(frm, text=str(self.steps.get()))
        self.lbl_steps.grid(row=0, column=2)

        ttk.Label(frm, text="TP (十六进制)").grid(row=1, column=0, sticky="w")
        self.s_tp = ttk.Scale(frm, from_=0x01, to=0x30, variable=self.tp,
                              command=lambda _: self._lbl_tp())
        self.s_tp.grid(row=1, column=1, sticky="ew")
        self.lbl_tp = ttk.Label(frm, text=f"0x{self.tp.get():02X}")
        self.lbl_tp.grid(row=1, column=2)

        ttk.Label(frm, text="边缘过渡 (px)").grid(row=2, column=0, sticky="w")
        self.s_feather = ttk.Scale(frm, from_=0, to=32, variable=self.feather,
                                   command=lambda _: self._lbl_feather())
        self.s_feather.grid(row=2, column=1, sticky="ew")
        self.lbl_feather = ttk.Label(frm, text=str(self.feather.get()))
        self.lbl_feather.grid(row=2, column=2)

        ttk.Label(frm, text="固化相位数").grid(row=3, column=0, sticky="w")
        self.s_settle = ttk.Scale(frm, from_=1, to=4, variable=self.settle_phases,
                                  command=lambda _: self._lbl_settle())
        self.s_settle.grid(row=3, column=1, sticky="ew")
        self.lbl_settle = ttk.Label(frm, text=str(self.settle_phases.get()))
        self.lbl_settle.grid(row=3, column=2)

        ttk.Label(frm, text="方向").grid(row=4, column=0, sticky="w")
        ttk.Radiobutton(frm, text="向前（新页从右进入）", variable=self.direction,
                        value="forward").grid(row=4, column=1, sticky="w")
        ttk.Radiobutton(frm, text="向后", variable=self.direction,
                        value="backward").grid(row=5, column=1, sticky="w")

        ttk.Checkbutton(frm, text="电压锁定（安全）",
                        state="disabled").grid(row=6, column=1, sticky="w")

        frm = ttk.LabelFrame(self.root, text="操作")
        frm.pack(fill="x", **pad)
        ttk.Button(frm, text="▶ 运行动画", command=self._run_animation).grid(row=0, column=0, **pad)
        ttk.Button(frm, text="固化", command=self._run_settle).grid(row=0, column=1, **pad)
        ttk.Button(frm, text="动画+固化", command=self._run_both).grid(row=0, column=2, **pad)
        ttk.Button(frm, text="安全恢复", command=self._full_clear).grid(row=1, column=0, **pad)
        ttk.Button(frm, text="重连设备", command=self._connect).grid(row=1, column=1, **pad)
        ttk.Button(frm, text="重启 daemon", command=self._restart_daemon).grid(row=1, column=2, **pad)
        ttk.Button(frm, text="重启 daemon", command=self._restart_daemon).grid(row=1, column=2, **pad)

        frm = ttk.LabelFrame(self.root, text="状态")
        frm.pack(fill="x", **pad)
        ttk.Label(frm, textvariable=self.status, wraplength=560).pack(fill="x")

        self.log = tk.Text(self.root, height=14, width=72)
        self.log.pack(fill="both", expand=True, **pad)

    def _lbl_steps(self): self.lbl_steps.config(text=str(self.steps.get()))
    def _lbl_tp(self): self.lbl_tp.config(text=f"0x{self.tp.get():02X}")
    def _lbl_feather(self): self.lbl_feather.config(text=str(self.feather.get()))
    def _lbl_settle(self): self.lbl_settle.config(text=str(self.settle_phases.get()))

    # ---------- device ----------
    def _connect(self):
        self._log("正在连接 m4adb daemon...")
        threading.Thread(target=self._connect_worker, daemon=True).start()
    def _connect_worker(self):
        try:
            c = Client(DaemonTransport(socket_path_for_port("/dev/cu.usbmodem101"), timeout=3.0),
                       default_timeout=15)
            c.wait_ready(timeout=30)
            with self.lock:
                self.client = c
            self._set_status("已连接")
            self._log("已连接（daemon socket）")
        except Exception as e:  # noqa: BLE001
            self._set_status(f"连接失败: {e}")

    def _restart_daemon(self):
        """Kill any (stale) m4adb daemon, clear sockets, spawn the one new
        daemon, then reconnect.  Only safe because the GUI is the sole
        device client; never run this while another tool holds the socket."""
        self._log("重启 m4adb daemon...")
        threading.Thread(target=self._restart_daemon_worker, daemon=True).start()

    def _restart_daemon_worker(self):
        import subprocess
        import time as _time
        port = "/dev/cu.usbmodem101"
        sock = socket_path_for_port(port)
        try:
            with self.lock:
                if self.client is not None:
                    try:
                        self.client.close()
                    except Exception:  # noqa: BLE001
                        pass
                    self.client = None
            # Kill stale daemons (they hold a dead serial handle after reset).
            subprocess.run(["pkill", "-f", "m4adb.py.*daemon"], capture_output=True)
            _time.sleep(1.5)
            for stale in Path("/tmp").glob("m4adb-*.sock"):
                try:
                    stale.unlink()
                except OSError:
                    pass
            # Spawn the single new daemon via the same command m4adb uses.
            py = sys.executable
            log = open(REPO / "build" / "m4adb" / "daemon.log", "a", encoding="utf-8")
            subprocess.Popen(
                [py, str(REPO / "scripts" / "m4adb.py"), "--port", port,
                 "daemon", "--ready-timeout", "90"],
                stdout=log, stderr=log, start_new_session=True, close_fds=True,
            )
            self._set_status("daemon 已重启，等待就绪...")
            deadline = _time.time() + 45
            while _time.time() < deadline:
                try:
                    c = Client(DaemonTransport(sock, timeout=2.0), default_timeout=15)
                    c.wait_ready(timeout=10)
                    with self.lock:
                        self.client = c
                    self._set_status("已连接（daemon 重启后）")
                    self._log("daemon 重启成功并已连接")
                    return
                except Exception:  # noqa: BLE001
                    _time.sleep(1)
            self._set_status("daemon 重启后仍无法连接（设备可能未唤醒）")
        except Exception as e:  # noqa: BLE001
            self._set_status(f"重启 daemon 失败: {e}")

    def _client(self) -> Client:
        with self.lock:
            return self.client

    def _req(self, obj: dict, timeout: float = 120) -> dict:
        """Request with one auto-reconnect retry: the device USB CDC can
        briefly stall after a long refresh; a stale daemon must be restarted
        once instead of failing the whole operation."""
        for attempt in range(2):
            c = self._client()
            if c is None:
                raise RuntimeError("未连接")
            try:
                return c.request(obj, timeout=timeout)
            except Exception as e:  # noqa: BLE001
                if attempt == 0 and "disconnect" in str(e).lower():
                    self._log("连接断开，自动重启 daemon 并重试...")
                    self._restart_daemon()
                    # wait for reconnect
                    for _ in range(40):
                        time.sleep(0.5)
                        if self._client() is not None:
                            break
                    else:
                        raise RuntimeError("重启 daemon 后仍无法连接")
                    continue
                raise
        raise RuntimeError("请求失败")

    def _upload_lut(self, lut: bytes, tag: str):
        self._req({"op": "lut_upload", "lut": b64(lut), "unlock_voltages": False}, timeout=15)
        self._log(f"[{tag}] LUT 已上传 ({len(lut)}B, 电压锁定安全)")

    # ---------- actions (worker threads so UI stays live) ----------
    def _run_animation(self):
        threading.Thread(target=self._anim_worker, daemon=True).start()

    def _anim_worker(self):
        """Run the animation exactly like the CLI does: one subprocess per
        step command (fresh daemon connection each time), which is the only
        path verified stable on the device."""
        try:
            prev, nxt = self.prev_path.get(), self.next_path.get()
            steps = self.steps.get()
            tp = self.tp.get()
            feather = self.feather.get()
            backward = self.direction.get() == "backward"
            if backward:
                prev, nxt = nxt, prev
            self._set_status("上传 LUT...")
            out = self._cli(["lut", "--hex", self._lut_hex(build_step_lut(tp))])
            self._log(out)
            self._set_status("刷新旧页基线 (FULL)...")
            self._cli(["baseline", prev])
            time.sleep(2)  # beat after FULL refresh
            self._set_status(f"擦入动画 {steps} 步 (TP=0x{tp:02X}, 过渡={feather}px)...")
            t0 = time.time()
            out = self._cli(["wipe", prev, nxt, "--steps", str(steps), "--feather", str(feather)])
            self._log(out)
            self._set_status(f"动画完成: {out.strip()} "
                             f"(墙钟 {(time.time()-t0)*1000:.0f} 毫秒)")
        except Exception as e:  # noqa: BLE001
            self._set_status(f"动画错误: {e}")
            self._log(f"动画错误: {e}")

    def _lut_hex(self, lut: bytes) -> str:
        return ",".join(f"{v:02X}" for v in lut)

    def _cli(self, args: list) -> str:
        """Run one waveform_lab.py CLI command as a subprocess (fresh
        daemon connection, identical to the verified CLI path)."""
        import subprocess
        cmd = [sys.executable, str(REPO / "scripts" / "waveform_lab.py"),
               "--port", "/dev/cu.usbmodem101"] + args
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr.strip() or proc.stdout.strip() or "CLI 失败")
        return proc.stdout.strip()

    def _run_settle(self):
        threading.Thread(target=self._settle_worker, daemon=True).start()

    def _settle_worker(self):
        try:
            prev, nxt = self.prev_path.get(), self.next_path.get()
            if self.direction.get() == "backward":
                prev, nxt = nxt, prev
            self._set_status("上传 SETTLE LUT...")
            self._cli(["lut", "--hex", self._lut_hex(
                build_settle_lut(self.settle_phases.get(), self.tp.get()))])
            self._set_status(f"固化中 ({self.settle_phases.get()} 相位)...")
            out = self._cli(["settle", prev, nxt])
            self._set_status(f"固化完成: {out.strip()}")
            self._log(out)
        except Exception as e:  # noqa: BLE001
            self._set_status(f"固化错误: {e}")
            self._log(f"固化错误: {e}")

    def _run_both(self):
        threading.Thread(target=self._both_worker, daemon=True).start()

    def _both_worker(self):
        self._anim_worker()
        time.sleep(0.5)
        self._settle_worker()

    def _full_clear(self):
        threading.Thread(target=self._clear_worker, daemon=True).start()

    def _clear_worker(self):
        try:
            prev = self.prev_path.get()
            if self.direction.get() == "backward":
                prev = self.next_path.get()
            self._set_status("全刷恢复到旧页...")
            self._req({"op": "lut_baseline", "frame": prev}, timeout=120)
            self._set_status("已恢复")
            self._log("安全恢复完成")
        except Exception as e:  # noqa: BLE001
            self._set_status(f"恢复错误: {e}")

    def _set_status(self, s: str):
        self.root.after(0, lambda: self.status.set(s))

    def _log(self, s: str):
        self.root.after(0, lambda: self.log.insert("end", s + "\n"))


def main() -> None:
    root = tk.Tk()
    WaveformLabGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
