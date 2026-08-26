#!/usr/bin/env python3
"""Slow-TTF synthetic input busy-reject regression (Phase 4).

Product contract: while the reader is in the slow loading / first-page index /
yield-busy window, synthetic page-turn inputs (tap / key / swipe) are rejected
or ignored and MUST NOT be queued for later replay. Once that window finishes,
a single normal page-turn works immediately (exactly once).

Evidence:
  1) Burst page-turn inputs during the busy window → zero later page turns
     (perf max page stays 0 after the window settles).
  2) One page-turn after the window → final page == 1.

Does not wait on the late-flushed ``first_page_index_begin`` serial line; uses
an immediate post-open burst plus settle-until-ready for the post-window check.
"""
from __future__ import annotations

import argparse
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
            "避免墨水屏连续重绘；加载中的翻页输入必须被丢弃且不事后回放。"
        )
    fixture.write_text("\n".join(lines) + "\n", encoding="utf-8")
    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(fixture), f"::/{FIXTURE_NAME}"],
        capture_output=True, text=True,
    )
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy fixture failed ({cp.returncode}):\n{cp.stdout}{cp.stderr}")
    return fixture


def _raw_key(client: Client, name: str, *, timeout: float = 15.0) -> dict:
    rid = client._next_id()
    client.t.write(build_req(rid, {"op": "key", "name": name}))
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
    raise BridgeError("timeout", f"key {name} no reply within {timeout}s")


def _try_page_turn_key(client: Client) -> dict:
    """One right-key attempt; busy/reject becomes a structured dict."""
    try:
        ack = _raw_key(client, "right", timeout=12.0)
        return {"ok": True, "ack": ack, "deferred": ack.get("deferred") is True}
    except BridgeError as exc:
        return {"ok": False, "error": exc.key, "message": exc.message, "deferred": False}


def _read_perf_tail(client: Client) -> str:
    import base64 as b64mod
    acc: list[str] = []
    offset: int | None = -1
    for _ in range(6):
        res = client.sd_read(PERF_LOG, offset=-1 if offset == -1 else offset, max_bytes=400)
        chunk = b64mod.b64decode(res.get("data_b64", "")).decode("utf-8", errors="replace")
        acc.append(chunk)
        if res.get("eof", True):
            break
        offset = int(res.get("offset", 0)) + int(res.get("n", 0))
    return "".join(acc)


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


def _wait_reader_settled(client: Client, proc: Any, qlog: Path, open_at: float, *, seconds: float) -> None:
    """Wait until TxtReader is up and the first-page index window has ended."""
    deadline = time.monotonic() + seconds
    saw_reader = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(f"QEMU exited waiting for reader settle\n{_qemu_tail(qlog)}")
        try:
            path = _activity_path(_ui(client))
        except BridgeError:
            time.sleep(0.25)
            continue
        if path[: len(READER_PATH)] == READER_PATH:
            saw_reader = True
        serial = "\n".join(client.serial_log[-2000:])
        q = _qemu_tail(qlog, 6000)
        end_seen = ("first_page_index_end" in serial) or ("first_page_index_end" in q)
        # Also accept perf evidence of page 0 once the reader exists.
        try:
            perf = _read_perf_tail(client)
            page0 = _max_delivered_page(perf) >= 0 and "page=0" in perf
        except BridgeError:
            page0 = False
        if saw_reader and (end_seen or page0) and (time.monotonic() - open_at) >= 8.0:
            print(
                f"reader settled at +{time.monotonic() - open_at:.3f}s "
                f"path={path!r} end={end_seen} page0={page0}",
                flush=True,
            )
            return
        time.sleep(0.4)
    raise m4sim.M4SimError(
        f"reader never settled within {seconds}s after open; saw_reader={saw_reader}\n"
        + _qemu_tail(qlog)
    )


def _run(base: Path, qemu: Path, flash: Path, font: Path, ready_seconds: float) -> dict[str, Any]:
    root = base / "synth-burst"
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    _set_session(root)
    root.mkdir(parents=True, exist_ok=True)

    journey_flash = root / "flash-16m.bin"
    shutil.copy2(flash, journey_flash)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)

    mmd = shutil.which("mmd")
    mcopy = shutil.which("mcopy")
    if not (mmd and mcopy):
        raise m4sim.M4SimError("mtools (mmd/mcopy) required for font install")
    subprocess.run([mmd, "-i", str(sd), "::/FONT"], check=False, capture_output=True)
    cp = subprocess.run([mcopy, "-o", "-i", str(sd), str(font), "::/FONT/M4Qemu.ttf"],
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

        # Immediate post-open burst: overlaps yield-busy open and/or the early
        # first-page index lock window without waiting on a late begin log.
        open_at = time.monotonic()
        client.key("confirm")
        print(f"reader open confirm at {open_at:.1f}s host", flush=True)

        n_burst = 8
        burst_results: list[dict] = []
        burst_start = time.monotonic()
        print(f"busy-window burst start at +{burst_start - open_at:.3f}s", flush=True)
        for i in range(n_burst):
            t0 = time.monotonic()
            result = _try_page_turn_key(client)
            burst_results.append(result)
            print(
                f"burst {i} at +{t0 - open_at:.3f}s ok={result.get('ok')} "
                f"err={result.get('error')} deferred={result.get('deferred')}",
                flush=True,
            )
            time.sleep(0.2)
        print(
            f"busy-window burst done at +{time.monotonic() - open_at:.3f}s "
            f"({time.monotonic() - burst_start:.1f}s)",
            flush=True,
        )
        if any(r.get("deferred") is True for r in burst_results):
            raise m4sim.M4SimError(
                f"deferred:true ACK must not occur under reject-busy policy; "
                f"burst={burst_results!r}"
            )

        _wait_reader_settled(client, proc, qlog, open_at, seconds=120.0)

        # After the window: burst inputs must not have produced later page turns.
        settle_deadline = time.monotonic() + 25.0
        max_after_burst = -1
        perf_text = ""
        while time.monotonic() < settle_deadline:
            if proc.poll() is not None:
                raise m4sim.M4SimError(f"QEMU exited during post-burst settle\n{_qemu_tail(qlog)}")
            try:
                perf_text = _read_perf_tail(client)
                max_after_burst = max(max_after_burst, _max_delivered_page(perf_text))
            except BridgeError:
                time.sleep(1.0)
                continue
            print(f"post-burst max_page={max_after_burst} at +{time.monotonic() - open_at:.1f}s", flush=True)
            if max_after_burst > 0:
                break
            time.sleep(1.0)
        if max_after_burst > 0:
            raise m4sim.M4SimError(
                f"busy-window inputs must not replay later: want max page 0, saw {max_after_burst}; "
                f"perf:\n{perf_text[-800:]}"
            )
        # page == -1 (no perf yet) or 0 is acceptable; normalize to 0 for report.
        if max_after_burst < 0:
            max_after_burst = 0

        # One normal page-turn after the window → exactly page 1.
        post = _try_page_turn_key(client)
        print(f"post-window turn ok={post.get('ok')} err={post.get('error')}", flush=True)
        if not post.get("ok"):
            # Brief retry once if the first hit a residual busy.
            time.sleep(0.5)
            post = _try_page_turn_key(client)
            print(f"post-window retry ok={post.get('ok')} err={post.get('error')}", flush=True)
        if not post.get("ok"):
            raise m4sim.M4SimError(f"post-window page-turn failed: {post!r}")
        if post.get("deferred") is True:
            raise m4sim.M4SimError(f"post-window turn must not be deferred: {post!r}")

        deadline = time.monotonic() + 60.0
        final_page = max_after_burst
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise m4sim.M4SimError(f"QEMU exited waiting for page 1\n{_qemu_tail(qlog)}")
            try:
                perf_text = _read_perf_tail(client)
                final_page = _max_delivered_page(perf_text)
            except BridgeError:
                time.sleep(1.0)
                continue
            print(f"post-turn max_page={final_page} target=1", flush=True)
            if final_page >= 1:
                break
            time.sleep(1.0)
        else:
            raise m4sim.M4SimError(
                f"post-window turn did not reach page 1; max={final_page}; perf:\n{perf_text[-800:]}"
            )
        if final_page != 1:
            raise m4sim.M4SimError(
                f"exact-once after window violated: want page 1, saw {final_page}"
            )

        busy_rejects = sum(1 for r in burst_results if r.get("error") == "busy")
        accepted_during_busy = sum(1 for r in burst_results if r.get("ok"))
        result = {
            "ok": True,
            "burst_inputs": n_burst,
            "busy_rejects": busy_rejects,
            "accepted_during_busy_window": accepted_during_busy,
            "max_page_after_burst": max_after_burst,
            "final_page": final_page,
            "burst_results": burst_results,
            "post_window_turn": post,
            "fixture": fixture.name,
        }
        (root / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(
            f"SYNTH BURST PASS: busy_rejects={busy_rejects}/{n_burst} "
            f"max_after_burst={max_after_burst} final_page={final_page}",
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
        description="Slow-TTF synthetic input busy-reject regression (Phase 4).",
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
