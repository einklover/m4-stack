#!/usr/bin/env python3
"""Fail-fast m4adb / QEMU smoke helpers.

Why this exists
---------------
Agent and manual loops used to:
  * sleep 50–70s hoping discovery finished
  * call m4adb with soft timeouts while guest was frozen (CPU 100%)
  * race ``run_plugin_debug``'s post-ready ``wifi_status`` on the same PTY

Result: multi-minute hangs that looked like "the tool is stuck".

Rules
-----
1. Every m4adb invocation has a **wall-clock** kill (subprocess timeout).
2. Default per-command budget is short (8s); overall budgets are capped.
3. Wait for ``keep-alive`` / session banner before using the PTY (not only
   ``bridge ready``, which prints before wifi_status releases the port).
4. On first unresponsive ping after launch → **FAIL FAST**, do not poll for
   minutes.

Examples::

  # Against an already-running plugin-debug session:
  python3 simulator/qemu/m4_quick.py --pty /dev/ttys006 ping
  python3 simulator/qemu/m4_quick.py --pty /dev/ttys006 fanqie

  # Or auto-read PTY from artifacts:
  python3 simulator/qemu/m4_quick.py fanqie
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
M4ADB = ROOT / "firmware/scripts/m4adb.py"
ART = Path(os.environ.get("M4_PLUGIN_DEBUG_TMP", "/tmp/m4-plugin-debug")) / "artifacts"
DEFAULT_CMD_S = 8.0
DEFAULT_OVERALL_S = 45.0


class FailFast(SystemExit):
    pass


def _pty_from_artifacts() -> str:
    p = ART / "pty.txt"
    if not p.is_file():
        raise FailFast(f"no PTY file at {p}; start run_plugin_debug --keep-alive first")
    return p.read_text(encoding="utf-8").strip()


def m4adb_hard(
    pty: str,
    args: list[str],
    *,
    cmd_s: float = DEFAULT_CMD_S,
    ready_s: float | None = None,
) -> tuple[int, str]:
    """Run one m4adb command; kill the process if wall clock exceeds cmd_s."""
    ready = ready_s if ready_s is not None else min(cmd_s, 6.0)
    cmd = [
        sys.executable,
        str(M4ADB),
        "--port",
        pty,
        "--no-daemon",
        "--timeout",
        str(max(1.0, cmd_s - 1.0)),
        "--ready-timeout",
        str(max(0.5, ready)),
        *args,
    ]
    print("+", " ".join(cmd), flush=True)
    try:
        cp = subprocess.run(
            cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=cmd_s,
        )
        out = cp.stdout or ""
        print(out, end="" if out.endswith("\n") or not out else "\n", flush=True)
        return cp.returncode, out
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        msg = f"HARD_TIMEOUT {cmd_s:.0f}s: m4adb {' '.join(args)}\n{out}"
        print(msg, flush=True)
        return 124, msg


def _json_blob(text: str) -> dict | None:
    m = re.search(r"\{[\s\S]*\}\s*$", text.strip())
    if not m:
        # first object in output
        start = text.find("{")
        if start < 0:
            return None
        try:
            return json.loads(text[start:])
        except json.JSONDecodeError:
            return None
    try:
        return json.loads(m.group(0))
    except json.JSONDecodeError:
        return None


def cmd_ping(pty: str, cmd_s: float) -> int:
    rc, out = m4adb_hard(pty, ["ping"], cmd_s=cmd_s)
    if rc != 0 or '"protocol"' not in out:
        print("FAIL: ping", flush=True)
        return 1
    print("OK: ping", flush=True)
    return 0


def cmd_fanqie(pty: str, cmd_s: float, overall_s: float) -> int:
    """Launch fanqie and check UI once; fail fast if guest dies."""
    t0 = time.time()
    deadline = t0 + overall_s

    def budget() -> float:
        return max(2.0, min(cmd_s, deadline - time.time()))

    if time.time() > deadline:
        raise FailFast("overall budget exhausted before start")

    rc, out = m4adb_hard(pty, ["ping"], cmd_s=budget())
    if rc != 0 or '"protocol"' not in out:
        print("FAIL: guest not responsive before launch", flush=True)
        return 1

    rc, out = m4adb_hard(pty, ["launch", "com.fanqie.client"], cmd_s=budget())
    if rc != 0 and '"app_id"' not in out:
        print("FAIL: launch", flush=True)
        return 1

    # Single short settle — do NOT sleep 60s.
    settle = min(5.0, max(0.0, deadline - time.time() - cmd_s))
    if settle > 0:
        print(f"settle {settle:.1f}s (capped)", flush=True)
        time.sleep(settle)

    rc, out = m4adb_hard(pty, ["ping"], cmd_s=budget())
    if rc != 0 or '"protocol"' not in out:
        print("FAIL: guest frozen after launch (discovery/TLS hang?)", flush=True)
        return 2

    rc, out = m4adb_hard(pty, ["ui"], cmd_s=budget())
    blob = _json_blob(out) or {}
    body = (((blob.get("ui") or {}).get("body")) or {})
    rows = body.get("rows")
    err = body.get("error") or ""
    activity = blob.get("activity") or body.get("kind")
    print(
        f"ui activity={activity!r} rows={rows!r} error={err!r} elapsed={time.time() - t0:.1f}s",
        flush=True,
    )
    if rows is None:
        print("FAIL: no ui body", flush=True)
        return 1
    if isinstance(rows, int) and rows > 0:
        print(f"OK: fanqie list rows={rows}", flush=True)
        return 0
    # Empty list is a soft fail — app opened but discovery empty/failed.
    print("SOFT_FAIL: fanqie open but rows=0 (check status text / discovery)", flush=True)
    return 3


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pty", default=None, help="serial PTY (default: artifacts/pty.txt)")
    p.add_argument("--cmd-timeout", type=float, default=DEFAULT_CMD_S, help="per m4adb wall-clock seconds")
    p.add_argument("--overall-timeout", type=float, default=DEFAULT_OVERALL_S, help="fanqie overall budget")
    p.add_argument(
        "action",
        choices=("ping", "fanqie", "ui"),
        help="ping | fanqie (launch+ui fail-fast) | ui",
    )
    args = p.parse_args(argv)
    pty = args.pty or _pty_from_artifacts()
    if not Path(pty).exists():
        raise FailFast(f"PTY missing: {pty} (QEMU dead?)")

    if args.action == "ping":
        return cmd_ping(pty, args.cmd_timeout)
    if args.action == "ui":
        rc, out = m4adb_hard(pty, ["ui"], cmd_s=args.cmd_timeout)
        return 0 if rc == 0 and "{" in out else 1
    if args.action == "fanqie":
        return cmd_fanqie(pty, args.cmd_timeout, args.overall_timeout)
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FailFast as e:
        print(str(e) or "FAIL", file=sys.stderr)
        raise SystemExit(1) from e
