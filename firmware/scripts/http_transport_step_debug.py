#!/usr/bin/env python3
"""Step-debug M4HttpTransport base using WeRead-shaped requests.

Does NOT launch the native reader UI. Each step is one m4adb http_probe call
so a panic/timeout can be attributed to a single phase:

  1. mem              heap / TLS gate snapshot
  2. debug_on         Serial + SD log (/apps_data/.../http_transport.log)
  3. session_begin    esp_http_client_init only (no network yet)
  4. tls_get          first HTTPS (handshake) — oneshot or session
  5. weread_psvts     GET reader HTML, extract psvts (needs login cookie on SD)
  6. weread_e0        POST e_0 shard
  7. weread_t0/t1 or e1/e3 depending on e0 prefix
  8. session_end / shutdown

Usage examples:
  # Base only (no WeRead account needed for steps 1–4):
  python3 scripts/http_transport_step_debug.py --port /dev/cu.usbmodem101 --base-only

  # Full WeRead chapter path (cookie must already be on device):
  python3 scripts/http_transport_step_debug.py --port /dev/cu.usbmodem101 \\
      --book-id 3300023282 --chapter-uid 1

  # Single step:
  python3 scripts/m4adb.py http_probe mem
  python3 scripts/m4adb.py http_probe tls_get --no-session
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from m4adb import _open_client  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402


def _print_step(name: str, res: dict) -> None:
    ok = res.get("ok")
    err = res.get("error") or ""
    b = res.get("before") or {}
    a = res.get("after") or {}
    print(
        f"\n=== {name} ok={ok} status={res.get('status')} bytes={res.get('bytes')} "
        f"err={err!r} session_open={res.get('session_open')}",
        flush=True,
    )
    print(f"    detail: {res.get('detail')}", flush=True)
    if res.get("psvts"):
        print(f"    psvts:  {res.get('psvts')}", flush=True)
    print(
        f"    before heap={b.get('heap')} int={b.get('int')} larg={b.get('larg')} tls={b.get('tls')}",
        flush=True,
    )
    print(
        f"    after  heap={a.get('heap')} int={a.get('int')} larg={a.get('larg')} tls={a.get('tls')}",
        flush=True,
    )


def main() -> int:
    p = argparse.ArgumentParser(description="M4HttpTransport step debugger (WeRead-shaped)")
    p.add_argument("--port", default=None)
    p.add_argument("--timeout", type=float, default=15.0)
    p.add_argument("--probe-timeout", type=float, default=90.0)
    p.add_argument("--base-only", action="store_true", help="Stop after tls_get (no cookie)")
    p.add_argument("--oneshot-first", action="store_true", help="tls_get oneshot before session")
    p.add_argument("--book-id", default="")
    p.add_argument("--chapter-uid", default="")
    p.add_argument("--stop-on-fail", action="store_true", default=True)
    p.add_argument("--continue-on-fail", action="store_true")
    p.add_argument("--wifi-prepare", action="store_true", default=True)
    p.add_argument("--no-wifi-prepare", action="store_true")
    args = p.parse_args()
    stop_on_fail = not args.continue_on_fail

    # Build a minimal namespace for _open_client.
    class NS:
        pass

    ns = NS()
    ns.port = args.port
    ns.baud = 115200
    ns.timeout = args.timeout
    ns.mock = False
    ns.no_daemon = False

    c = _open_client(ns, ready_timeout=30.0)
    try:
        if args.wifi_prepare and not args.no_wifi_prepare:
            print("[step] wifi_prepare…", flush=True)
            try:
                w = c.wifi_prepare(timeout=45.0)
                print(json.dumps(w, ensure_ascii=False), flush=True)
            except BridgeError as e:
                print(f"wifi_prepare failed: {e.key}: {e.message}", file=sys.stderr)
                return 1

        def step_ok(step: str, res: dict) -> bool:
            if res.get("ok"):
                return True
            # Older firmware tls_get capped at 64KB: status 200 + HTML still
            # proves TLS/handshake even when error=response_too_large.
            if step == "tls_get":
                st = int(res.get("status") or 0)
                n = int(res.get("bytes") or 0)
                err = str(res.get("error") or "")
                if 200 <= st < 300 and n > 0 and "response_too_large" in err:
                    return True
            return False

        def run(step: str, **kw) -> dict:
            print(f"\n>>> http_probe {step} {kw}", flush=True)
            t0 = time.time()
            res = c.http_probe(
                step,
                timeout=float(args.probe_timeout),
                timeout_ms=30000,
                book_id=kw.get("book_id", args.book_id),
                chapter_uid=kw.get("chapter_uid", args.chapter_uid),
                session=kw.get("session", True),
                url=kw.get("url", "https://weread.qq.com/"),
                host=kw.get("host", "weread.qq.com"),
            )
            res["_wall_s"] = round(time.time() - t0, 2)
            res["_step_ok"] = step_ok(step, res)
            _print_step(step, res)
            print(f"    wall_s={res['_wall_s']} step_ok={res['_step_ok']}", flush=True)
            return res

        # --- Base path ---
        r = run("mem")
        if not r.get("before", {}).get("tls", r.get("after", {}).get("tls", True)):
            print("WARNING: TLS gate already red (largest internal < 40KB)", flush=True)

        run("debug_on")
        r = run("session_begin")
        if not r.get("_step_ok") and stop_on_fail:
            return 2

        if args.oneshot_first:
            # Close session first so oneshot is clean; then re-begin.
            run("session_end")
            r = run("tls_get", session=False)
            if not r.get("_step_ok") and stop_on_fail:
                print("oneshot tls_get FAILED — base cannot complete HTTPS", flush=True)
                return 2
            run("session_begin")

        r = run("tls_get", session=True)
        if not r.get("_step_ok") and stop_on_fail:
            print(
                "session tls_get FAILED — handshake/perform is the suspect "
                "(compare with --oneshot-first)",
                flush=True,
            )
            return 2

        if args.base_only:
            run("session_end")
            print("\nBase-only path finished OK.", flush=True)
            return 0

        if not args.book_id or not args.chapter_uid:
            print(
                "\nNo --book-id/--chapter-uid: stop after base. "
                "Pass both to continue WeRead steps.",
                flush=True,
            )
            run("session_end")
            return 0

        r = run("weread_psvts")
        if not r.get("_step_ok") and stop_on_fail:
            return 2

        r = run("weread_e0")
        if not r.get("_step_ok") and stop_on_fail:
            return 2

        detail = str(r.get("detail") or "")
        # e0 body "{}" is soft-empty (auth/permission/protocol), not transport fail.
        if "pfx={}" in detail or (r.get("bytes") == 2 and "pfx={}" in detail):
            print(
                "NOTE: e0 returned empty JSON {} — typically login timeout or "
                "wrong book/chapter; TLS path is already proven.",
                flush=True,
            )
        # Heuristic: text mode e0 prefix often starts with '{"bookId"'
        text_mode = "bookId" in detail or ('pfx={"' in detail and "pfx={}" not in detail)
        if text_mode:
            for ep in ("weread_t0", "weread_t1"):
                r = run(ep)
                if not r.get("_step_ok") and stop_on_fail:
                    return 2
        else:
            for ep in ("weread_e1", "weread_e3"):
                r = run(ep)
                if not r.get("_step_ok") and stop_on_fail:
                    return 2

        run("session_end")
        print("\nWeRead-shaped step path finished.", flush=True)
        print(
            "SD log: apps_data/com.weread.client/logs/http_transport.log "
            "(m4adb sd_read …)",
            flush=True,
        )
        return 0
    except BridgeError as e:
        print(f"错误 {e.key}: {e.message}", file=sys.stderr)
        return 1
    finally:
        c.close()


if __name__ == "__main__":
    raise SystemExit(main())
