#!/usr/bin/env python3
"""Slow-TTF synthetic input burst regression (deferred-input delivery).

Reproduces the Phase 4 defect end to end: while the reader rasterizes a large
SD-resident TTF, the owner loop re-enters the debug bridge through
m4YieldToDebugBridge() (yield context). Synthetic taps sent in that window must
be ACKed with ``deferred:true``, queued FIFO, and delivered at most one per
regular frame — never silently lost.

Evidence contract:
  * every burst tap ACK carries ``deferred:true``
  * exactly N taps → exactly N page turns (final page == first page + N)
  * no tap is dropped by transient busy/rate-limit
"""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))

import m4sim  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402
from m4adb_lib.protocol import build_req  # noqa: E402
from m4adb_lib.transport import make_transport  # noqa: E402
from m4adb_observing_client import ObservingClient as Client  # noqa: E402

FIXTURE_NAME = "synth-burst.txt"
READER_PATH = ("Reader", "TxtReader")
PERF_LOG = "apps_data/com.jjwxc.client/logs/reader_perf.log"


def _set_session(root: Path) -> None:
    m4sim.DEFAULT_SESSION = root
    m4sim.ART = root / "artifacts"
    m4sim.STATE = root / "state.json"
    os.environ["M4SIM_TMP"] = str(root)


def _terminate(proc: Any) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()
        try:
            proc.wait(timeout=2)
        except Exception:
            pass


def _ui(client: Client) -> dict[str, Any]:
    return client.ui()


def _activity_path(ui: dict[str, Any]) -> tuple[str, ...]:
    path: list[str] = []
    top = ui.get("activity")
    if isinstance(top, str) and top:
        path.append(top)
    outer = ui.get("ui")
    body: Any = outer.get("body") if isinstance(outer, dict) else None
    for _ in range(8):
        if not isinstance(body, dict):
            break
        sub = body.get("subactivity")
        if isinstance(sub, str) and sub:
            path.append(sub)
        child = body.get("child")
        if not isinstance(child, dict):
            break
        body = child
    return tuple(path)


def _wait_path(client: Client, proc: Any, qlog: Path, expected: tuple[str, ...], *, seconds: float) -> dict:
    deadline = time.monotonic() + seconds
    last: dict = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            tail = qlog.read_text(errors="replace")[-2000:] if qlog.is_file() else ""
            raise m4sim.M4SimError(f"QEMU exited waiting for {expected!r}\n{tail}")
        try:
            last = _ui(client)
            if _activity_path(last)[: len(expected)] == expected:
                return last
        except BridgeError:
            pass
        time.sleep(0.2)
    raise m4sim.M4SimError(f"path never became {expected!r}; last={json.dumps(last, ensure_ascii=False)}")


def _qemu_tail(qlog: Path, chars: int = 3000) -> str:
    return qlog.read_text(encoding="utf-8", errors="replace")[-chars:] if qlog.is_file() else ""


def _seed_txt(sd: Path, root: Path) -> Path:
    mcopy = shutil.which("mcopy")
    mmd = shutil.which("mmd")
    if not (mcopy and mmd):
        raise m4sim.M4SimError("mtools (mcopy/mmd) required for SD fixture injection")
    # reader_perf.log is written under apps_data/com.jjwxc.client/logs; the
    # firmware appends with O_CREAT but never mkdirs — pre-create on the image.
    for d in ("::/apps_data", "::/apps_data/com.jjwxc.client",
              "::/apps_data/com.jjwxc.client/logs"):
        subprocess.run([mmd, "-i", str(sd), d], capture_output=True)
    fixture = root / FIXTURE_NAME
    lines = [
        "墨水屏阅读器 慢速TTF合成输入突发回归",
        "这是一份仅用于模拟器的 UTF-8 本地 TXT。",
        "",
    ]
    for i in range(1, 241):
        lines.append(
            f"第{i:03d}段  天地玄黄，宇宙洪荒。触控操作采用离散提交，"
            "避免墨水屏连续重绘；突发点击必须按序送达且不丢失。"
        )
    fixture.write_text("\n".join(lines) + "\n", encoding="utf-8")
    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(fixture), f"::/{FIXTURE_NAME}"],
        capture_output=True, text=True,
    )
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy fixture failed ({cp.returncode}):\n{cp.stdout}{cp.stderr}")
    return fixture


def _raw_tap(client: Client, x: int, y: int, *, timeout: float = 25.0, retries: int = 8) -> dict:
    """Raw tap preserving the full ok payload (deferred evidence). Retries the
    explicit busy rejection seen on the direct (non-yield) rate-limited path."""
    last_busy: BridgeError | None = None
    for attempt in range(retries):
        try:
            return _raw_tap_once(client, x, y, timeout=timeout)
        except BridgeError as exc:
            if exc.key != "busy":
                raise
            last_busy = exc
            time.sleep(0.06)
    raise last_busy or BridgeError("busy", "tap retries exhausted")


def _raw_tap_once(client: Client, x: int, y: int, *, timeout: float) -> dict:
    rid = client._next_id()
    client.t.write(build_req(rid, {"op": "tap", "x": int(x), "y": int(y)}))
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = client.t.read(timeout=0.1)
        if not data:
            continue
        for raw_line, frame in client.parser.feed(data):
            client._emit_log(raw_line)
            if frame is None or frame.req_id != rid:
                continue
            if frame.kind == "err":
                j = frame.json or {}
                raise BridgeError(j.get("error", "error"), j.get("message", ""))
            if frame.kind == "ok":
                return frame.json or {}
    raise BridgeError("timeout", f"tap ({x},{y}) no reply within {timeout}s")


def _read_perf_tail(client: Client) -> str:
    """Read the reader perf log via sd_read (tail). Returns decoded text."""
    import base64 as b64mod
    acc: list[str] = []
    offset: int | None = -1  # -1 = tail read
    for _ in range(6):
        res = client.sd_read(PERF_LOG, offset=-1 if offset == -1 else offset, max_bytes=400)
        chunk = b64mod.b64decode(res.get("data_b64", "")).decode("utf-8", errors="replace")
        acc.append(chunk)
        if res.get("eof", True):
            break
        offset = int(res.get("offset", 0)) + int(res.get("n", 0))
    return "".join(acc)


def _run(base: Path, qemu: Path, flash: Path, font: Path, ready_seconds: float) -> dict[str, Any]:
    root = base / "synth-burst"
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    _set_session(root)
    root.mkdir(parents=True, exist_ok=True)

    journey_flash = root / "flash-16m.bin"
    shutil.copy2(flash, journey_flash)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)

    # Install the slow external TTF at /FONT/M4Qemu.ttf — plugin-debug firmware
    # selects it at boot (SETTINGS.fontFamily=CUSTOM), so every reader glyph
    # raster streams from SD with m4YieldToDebugBridge() yields.
    mmd = shutil.which("mmd")
    mcopy = shutil.which("mcopy")
    if not (mmd and mcopy):
        raise m4sim.M4SimError("mtools (mmd/mcopy) required for font install")
    subprocess.run([mmd, "-i", str(sd), "::/FONT"], check=False, capture_output=True)
    cp = subprocess.run([mcopy, "-o", "-i", str(sd), str(font), f"::/FONT/M4Qemu.ttf"],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy font failed: {cp.stdout}{cp.stderr}")

    fixture = _seed_txt(sd, root)

    proc = None
    client: Client | None = None
    try:
        proc, pty, qlog = m4sim.boot_qemu(qemu, journey_flash, sd, open_eth=True, psram_mb=8)
        m4sim.wait_m4adb_ready(pty, proc, seconds=ready_seconds, qemu_log=qlog)
        client = Client(make_transport(pty), default_timeout=10.0)
        client.wait_ready(timeout=min(20.0, max(5.0, ready_seconds)))

        _wait_path(client, proc, qlog, ("Home",), seconds=45.0)
        client.key("confirm")
        _wait_path(client, proc, qlog, ("MyLibrary",), seconds=30.0)
        for _ in range(16):
            ui = _ui(client)
            body = ui.get("ui", {}).get("body", {})
            if isinstance(body, dict) and body.get("selected") == FIXTURE_NAME:
                break
            client.key("down")
            time.sleep(0.15)
        else:
            raise m4sim.M4SimError(f"library selection failed: {_activity_path(ui)!r}")
        client.key("confirm")

        # First open runs the known ~14.6 s first_page_index slow window
        # (SD TTF glyph raster with m4YieldToDebugBridge yields).  The old test
        # waited for perf step=loadPage, which is emitted AFTER the window has
        # finished, so every tap was direct and never exercised the deferred
        # path.  Fix: detect the observable serial marker first_page_index_begin
        # and burst INSIDE the window, so at least one ACK is deferred:true.
        # Poll both the QEMU log and the observing client's serial log; the
        # former captures firmware printfs regardless of m4adb request cadence,
        # the latter is pumped by lightweight ui polls during the yield window.
        begin_deadline = time.monotonic() + 90.0
        begin_seen = False
        begin_at = 0.0
        while time.monotonic() < begin_deadline:
            if proc.poll() is not None:
                raise m4sim.M4SimError(f"QEMU exited before first_page_index_begin\n{_qemu_tail(qlog)}")
            try:
                _ui(client)
            except BridgeError:
                pass
            serial_tail = "\n".join(client.serial_log[-3000:])
            qtail = _qemu_tail(qlog, 8000)
            if "first_page_index_begin" in serial_tail or "first_page_index_begin" in qtail:
                begin_seen = True
                begin_at = time.monotonic()
                print(f"first_page_index_begin seen at {begin_at:.1f}s host", flush=True)
                break
            time.sleep(0.2)
        if not begin_seen:
            serial_tail = "\n".join(client.serial_log[-2000:])
            raise m4sim.M4SimError(
                f"first_page_index_begin never appeared within 90s; "
                f"qtail={_qemu_tail(qlog, 600)!r} serial_tail={serial_tail[-600:]!r}"
            )

        # Burst INSIDE the slow window: all taps must land while the owner
        # loop is still inside buildPageIndexFirstPage / TTF raster yields
        # (m4YieldToDebugBridge yield-context). At least one must ACK
        # deferred:true; with short spacing the whole burst fits inside the
        # ~14.6 s window (6 * 0.25 s ≈ 1.5 s).
        n = 6
        acks: list[dict] = []
        burst_start = time.monotonic()
        print(f"burst start at {burst_start:.1f}s host (delta {burst_start-begin_at:.1f}s after begin)", flush=True)
        for i in range(n):
            t0 = time.monotonic()
            try:
                ack = _raw_tap(client, 400, 420, timeout=15.0)
            except BridgeError as e:
                ack = {"error": e.key, "message": e.message}
            acks.append(ack)
            print(f"tap {i} at {t0:.1f}s defer={ack.get('deferred')} op={ack.get('op')} err={ack.get('error')}", flush=True)
            if i < n - 1:
                time.sleep(0.25)
        print(f"burst done at {time.monotonic():.1f}s host, total {time.monotonic()-burst_start:.1f}s", flush=True)
        serial_after = "\n".join(client.serial_log[-3000:])
        if "first_page_index_end" in serial_after:
            print(f"first_page_index_end seen after burst", flush=True)
        deferred_acks = [a for a in acks if a.get("deferred") is True]
        direct_acks = [a for a in acks if a.get("deferred") is not True]
        if not deferred_acks:
            raise m4sim.M4SimError(
                f"no deferred:true tap ACK observed; acks={acks!r}\n" + _qemu_tail(qlog)
            )

        # Exact N-to-N: fresh reader sits on page 0; every tap — direct or
        # deferred, even when quick-tap coalesced — advances the target by
        # exactly 1. The reader logs loadPage/renderPage/anim with the page it
        # actually materialized; the maximum observed page must equal N.
        # A lost tap undershoots; a duplicated injection overshoots.
        target_page = n

        def _max_delivered_page(text: str) -> int:
            best = -1
            for line in text.splitlines():
                if "perf step=" not in line:
                    continue
                page_tok = None
                ch_ok = False
                for tok in line.split():
                    if tok.startswith("ch="):
                        ch_ok = tok[3:].isdigit() and int(tok[3:]) == 0
                    elif tok.startswith("page=") and tok[5:].isdigit():
                        page_tok = int(tok[5:])
                if ch_ok and page_tok is not None:
                    best = max(best, page_tok)
            return best

        deadline = time.monotonic() + 90.0
        final_pages: list[int] = []
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise m4sim.M4SimError(f"QEMU exited during delivery\n{_qemu_tail(qlog)}")
            try:
                perf_text = _read_perf_tail(client)
            except BridgeError:
                time.sleep(1.0)
                continue
            current_max = _max_delivered_page(perf_text)
            final_pages.append(current_max)
            print(f"perf poll max_page={current_max} target={target_page} at {time.monotonic():.1f}s", flush=True)
            if current_max >= target_page:
                break
            time.sleep(1.5)
        else:
            raise m4sim.M4SimError(
                f"delivery incomplete: want page {target_page}, max seen {max(final_pages) if final_pages else -1}; "
                f"perf tail:\n{perf_text[-800:]}"
            )
        if max(final_pages) != target_page:
            raise m4sim.M4SimError(
                f"exact-once violated: want page {target_page}, saw max {max(final_pages)}"
            )

        result = {
            "ok": True,
            "burst_taps": n,
            "ack_count": len(acks),
            "deferred_ack_count": len(deferred_acks),
            "direct_ack_count": len(direct_acks),
            "acks": acks,
            "final_page": max(final_pages),
            "expected_final_page": target_page,
            "fixture": fixture.name,
        }
        (root / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(
            f"SYNTH BURST PASS: taps={n} deferred_acks={len(deferred_acks)} "
            f"direct_acks={len(direct_acks)} final_page={max(final_pages)}",
            flush=True,
        )
        print(f"artifacts: {root}", flush=True)
        return result
    except BridgeError as exc:
        raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
    finally:
        artifacts = root / "artifacts"
        artifacts.mkdir(parents=True, exist_ok=True)
        if client is not None:
            (artifacts / "firmware-serial.log").write_text(
                "\n".join(client.serial_log) + "\n", encoding="utf-8"
            )
            client.close()
        _terminate(proc)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="m4sim test synth-burst",
        description="Slow-TTF synthetic input burst regression (deferred delivery).",
    )
    p.add_argument("--font", help="large TrueType to install as /FONT/M4Qemu.ttf "
                   "(default: kindle round-gothic CJK TTF if present)")
    p.add_argument("--qemu", help="explicit patched v3 qemu-system-xtensa")
    p.add_argument("--ready-seconds", type=float, default=150.0)
    p.add_argument("--skip-build", action="store_true", help="reuse existing plugin-debug build artifacts")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    font_arg = args.font
    if not font_arg:
        candidates = [
            Path("/Users/zhouxinlai/Downloads/kindle自带圆体2.ttf"),
            Path("/Library/Fonts/Arial Unicode.ttf"),
        ]
        font_arg = next((str(c) for c in candidates if c.is_file()), None)
    if not font_arg:
        print("SYNTH BURST FAIL: no CJK TTF font available for --font", file=sys.stderr)
        return 2

    base = Path(os.environ.get("M4SIM_SYNTH_TMP", "/tmp/m4sim-synth-burst"))
    if base.exists():
        shutil.rmtree(base, ignore_errors=True)
    base.mkdir(parents=True, exist_ok=True)

    try:
        qemu = m4sim.find_qemu(args.qemu, build_if_missing=True)
        build_root = base / "_build"
        _set_session(build_root)
        ns = argparse.Namespace(
            image=None, plugin_debug=True, build_dir=None, skip_build=bool(args.skip_build),
        )
        flash = m4sim.resolve_flash(ns)
        result = _run(base, qemu, flash, Path(font_arg), args.ready_seconds)
        (base / "synth-burst-summary.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        return 0
    except m4sim.M4SimError as exc:
        print(f"SYNTH BURST FAIL: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
