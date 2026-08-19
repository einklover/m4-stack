#!/usr/bin/env python3
"""Single-connection M3 Phase D lab sequence + m4adb snapshots.

Keeps one M4B3 TCP session so acquire/release does not force FirstBaseline
between sparse / dense / cadence / inject cases.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from m4b3_lab_client import (  # noqa: E402
    connect,
    crc32,
    dense_fb,
    encode_ping,
    fragmented_fb,
    hello_ok,
    landmark_fb,
    parse_ack,
    read_msg,
    send_key,
    sparse_fb,
    two_region_fb,
)

M4ADB = ROOT / "m4adb.py"


def m4adb(op: str) -> dict:
    out = subprocess.check_output(
        [sys.executable, str(M4ADB), op],
        stderr=subprocess.STDOUT,
        text=True,
    )
    start = out.find("{")
    if start < 0:
        raise RuntimeError(f"m4adb {op} produced no JSON: {out[:200]!r}")
    return json.loads(out[start:])


def reason_name(code: int) -> str:
    names = {
        0: "none",
        1: "first",
        2: "untrusted",
        3: "no_change",
        4: "sparse",
        5: "dense",
        6: "fragmented",
        7: "cadence_n",
        8: "cadence_area",
        9: "recover",
    }
    return names.get(int(code), str(code))


def show(label: str, panel: dict) -> None:
    print(
        f"[{label}] trusted={panel.get('trusted')} full={panel.get('full_ok')}/"
        f"{panel.get('full_req')}/{panel.get('full_err')} part={panel.get('partial_ok')}/"
        f"{panel.get('partial_req')}/{panel.get('partial_err')} n={panel.get('n')} "
        f"cum={panel.get('cum')} dirty={panel.get('dirty')} rects={panel.get('rects')} "
        f"reason={reason_name(panel.get('reason', 0))} full_ms={panel.get('full_ms')} "
        f"part_ms={panel.get('part_ms')} hyg={panel.get('hyg_ok')}/"
        f"{panel.get('hyg_req')}/{panel.get('hyg_err')} cov={panel.get('cov')} "
        f"hyg_ms={panel.get('hyg_ms')} src={panel.get('src_id')} "
        f"src_crc={panel.get('src_crc')} accepted={panel.get('accepted_crc')} "
        f"panel=0x{int(panel.get('panel_crc') or 0):08X} err={panel.get('err')} "
        f"win={panel.get('win')}",
        flush=True,
    )


def wait_idle(timeout: float = 12.0, src_id: int | None = None) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = m4adb("m4b3_panel")
        idle = (not last.get("busy")) and (not last.get("pending"))
        if idle and (src_id is None or int(last.get("src_id") or -1) == src_id):
            return last
        time.sleep(0.25)
    return last


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, default=48624)
    p.add_argument("--interval", type=float, default=2.3)
    p.add_argument("--sparse-count", type=int, default=32)
    args = p.parse_args()

    print("=== baseline snapshot ===", flush=True)
    show("pre", m4adb("m4b3_panel"))
    st = m4adb("m4b3_status")
    print(
        f"[pre-status] heap={st.get('free_heap')} min={st.get('min_free_heap')} "
        f"psram={st.get('free_psram')} reset={st.get('reset_reason')}",
        flush=True,
    )

    fid = 0
    with connect(args.host, args.port) as sock:
        hello_ok(sock)

        fb = bytes(landmark_fb())
        result, accepted, dt = send_key(sock, fid, fb, fid + 1)
        print(f"landmark ack result={result} accepted={accepted} ack_ms={dt:.1f} crc=0x{crc32(fb):08X}", flush=True)
        time.sleep(0.4)
        show("after-landmark", wait_idle(src_id=fid))

        partial_before = int(m4adb("m4b3_panel").get("partial_ok") or 0)
        for i in range(args.sparse_count):
            fid += 1
            fb = bytes(sparse_fb(i) if i % 5 != 3 else two_region_fb(i))
            result, accepted, dt = send_key(sock, fid, fb, fid + 1)
            print(
                f"sparse={i} id={fid} ack result={result} accepted={accepted} "
                f"ack_ms={dt:.1f} crc=0x{crc32(fb):08X}",
                flush=True,
            )
            if accepted != fid:
                return 2
            time.sleep(0.3)
            show(f"sparse-{i}", wait_idle(src_id=fid))

        panel = m4adb("m4b3_panel")
        partial_after = int(panel.get("partial_ok") or 0)
        print(f"partial_delta={partial_after - partial_before}", flush=True)

        time.sleep(args.interval)
        fid += 1
        fb = bytes(dense_fb())
        t0 = time.perf_counter()
        result, accepted, dt = send_key(sock, fid, fb, fid + 1)
        sock.sendall(encode_ping(seq=200, nonce=0xD00D))
        pong = read_msg(sock, timeout=3.0)
        ping_ms = (time.perf_counter() - t0) * 1000.0
        print(
            f"dense id={fid} ack result={result} accepted={accepted} ack_ms={dt:.1f} "
            f"crc=0x{crc32(fb):08X} ping_type={pong[4]} ping_after_ms={ping_ms:.1f}",
            flush=True,
        )
        show("after-dense", wait_idle(src_id=fid))

        fid += 1
        fb = bytes(fragmented_fb())
        result, accepted, dt = send_key(sock, fid, fb, fid + 1)
        print(f"frag id={fid} ack result={result} accepted={accepted} ack_ms={dt:.1f} crc=0x{crc32(fb):08X}", flush=True)
        show("after-frag", wait_idle(src_id=fid))

        # Re-baseline with a small frame, then inject a presenter failure.
        fid += 1
        fb = bytes(sparse_fb(100))
        result, accepted, dt = send_key(sock, fid, fb, fid + 1)
        print(f"pre-inject id={fid} ack={accepted} ack_ms={dt:.1f} crc=0x{crc32(fb):08X}", flush=True)
        before = wait_idle(src_id=fid)
        show("pre-inject", before)
        print(json.dumps(m4adb("m4b3_inject_fail"), ensure_ascii=False), flush=True)
        fid += 1
        fb = bytes(sparse_fb(101))
        crc_before = int(before.get("panel_crc") or 0)
        err_before = int(before.get("err") or 0)
        result, accepted, dt = send_key(sock, fid, fb, fid + 1)
        print(f"inject-frame id={fid} ack={accepted} ack_ms={dt:.1f} crc=0x{crc32(fb):08X}", flush=True)
        after_fail = {}
        for _ in range(20):
            after_fail = m4adb("m4b3_panel")
            if int(after_fail.get("err") or 0) > err_before:
                break
            time.sleep(0.15)
        show("inject-fail", after_fail)
        time.sleep(args.interval)
        after_rec = wait_idle()
        show("auto-recover", after_rec)
        print(
            f"inject_crc_unchanged={int(after_fail.get('panel_crc') or 0) == crc_before} "
            f"inject_err_delta={int(after_fail.get('err') or 0) - err_before} "
            f"recover_trusted={after_rec.get('trusted')} recover_reason={reason_name(after_rec.get('reason', 0))} "
            f"recover_full={after_rec.get('full_ok')}",
            flush=True,
        )

    print("=== disconnect / ownership ===", flush=True)
    time.sleep(1.5)
    show("after-disconnect", m4adb("m4b3_panel"))
    st = m4adb("m4b3_status")
    print(
        f"[post-status] heap={st.get('free_heap')} min={st.get('min_free_heap')} "
        f"psram={st.get('free_psram')} owner={st.get('panel_owner')} "
        f"accepted={st.get('accepted_crc')}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
