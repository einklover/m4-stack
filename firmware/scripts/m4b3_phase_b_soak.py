#!/usr/bin/env python3
"""Phase B Browser Bridge device-soak checklist (deferred until #34 optical gate).

Host-only default: prints the checklist and exits 0. Does not touch M4, Android,
serial, or m4adb unless ALL of these are passed:

  --execute --after-optical-gate --i-understand-device-mutation

Even then this file is a structured operator/agent checklist plus optional
wrappers around existing tools. It must not be run from host-build-only tasks.

Issue: einklover/m4-stack#35
Baseline head: 821acd8b71464032304b1d90e1ca27c29a7d8320
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
M4ADB = ROOT / "m4adb.py"
PHASE_D = ROOT / "m4b3_phase_d_run.py"
FLASH = ROOT / "flash_app1_once.sh"

CHECKLIST = """
# Phase B — Browser Bridge device soak (after #34 optical gate)

Do not start this until a human has confirmed #34 optical acceptance on the
currently flashed Murphy glass (HUD/BTN_A taps stay Partial, no extra FULL
flash). #35 Phase A is host/build-only and must leave both devices untouched.

Safety
- APP1-only if a new firmware is required; never APP0 / partition / bootloader
- no force-push, no custom LUT / voltage / waveform
- one device-mutating task at a time
- m4adb: single global daemon; do not pkill -f m4adb.py
- synthetic dumpsys / m4b3_lab_client input is transport stress only — never
  label it as real-finger / optical acceptance
- do not factory-reset, wipe data, or adb rm -rf

Preconditions
1. #34 optical gate closed on the flashed head (821acd8 or a later soak head).
2. Record exact firmware.bin sha256 / size and Android APK sha256 / size.
3. Confirm APP1 @ 0x6e0000 hash and OTA slot 1 if firmware was (re)flashed.
4. Confirm one healthy m4adb daemon; reuse it. Do not restart a live daemon.
5. Motorola Browser Bridge already running; do not restart it unless a
   continuity case explicitly requires it.

Record before every mutating step
- m4adb status: version, heap, min_heap, PSRAM, reset_reason, wifi_ip
- m4adb m4b3_panel: owner, trusted, epoch, full_ok/req/err, partial_ok/req/err,
  no_change, reason, dirty, rects, area, src_id, accepted_crc, panel_crc, err
- Android: dumpsys activity / BrowserBridge only if needed; no service restart

B1. Idle continuity (>=60s)
- owner stays BrowserBridge=2, trusted=true, epoch stable
- no extra presents (full_ok/partial_ok unchanged)
- no WDT / reset

B2. Sparse Partial (INPUT_TEST HUD / BTN_A / small glyph)
- expected reason=4 SparsePartial, dirty hundreds, rects<=4
- full_ok unchanged; partial_ok increments
- FirstBaseline / cadence_n after 8 Partials is allowed hygiene, not a fail

B3. Dense / fragmented / recover FULL still works
- dense page / 11-widget first paint: reason=5 or 6 Full
- m4b3_inject_fail then next frame: reason=9 recover Full, lastPresented
  not advanced on failure, then exactly one recovery Full before trust
- do not weaken these paths if sparse looks good

B4. ACK independence
- FRAME_ACK / src_id==accepted_id must complete without waiting for present
- send PING during an in-flight FULL; PONG must return while panel is busy
- existing helper: firmware/scripts/m4b3_phase_d_run.py --host <m4-ip>

B5. Reconnect / restart / reboot
- TCP drop: invalidate physical baseline; next present is ForcedFullRecovery
  if owner kept, or FirstBaseline after release/reacquire
- INPUT_TEST restart: reason=1 FirstBaseline Full, epoch bumps, no white glass
- device reboot: exactly one required FirstBaseline Full after connect;
  panel_crc non-zero; corners remain INPUT_TEST boxes if that page is showing
- repeat reconnect x5 and reboot x2; record reason distribution

B6. Doze / Android service continuity (only after optical gate)
- screen-off soak >=30 min; do not claim optical quality
- on wake: no extra untrusted Full storm; service still owns the TCP session
  or recovers with exactly one FirstBaseline
- do not treat this as #34 visual acceptance

B7. Heap / PSRAM / reset / WDT
- snapshot heap/min-heap/PSRAM before and after B1–B6
- reset_reason must stay the expected last flash/reboot value
- err=0 except the deliberate inject-fail case

Pass / fail
- PASS_AUTOMATED: counters + ACK/CRC + heap/PSRAM as above
- optical / real-finger remain a human gate if glass is in the loop
- do not close #35 until Phase B evidence is posted
""".strip()


def print_checklist() -> None:
    print(CHECKLIST)
    print()
    print("Existing helpers (later execution only):")
    print(f"  {PHASE_D} --host <m4-sta-ip>")
    print(f"  {FLASH} /dev/cu.usbmodem101   # APP1-only, only if firmware changed")
    print(f"  {M4ADB} status")
    print(f"  {M4ADB} m4b3_panel")
    print("Default of this script is print-only. Device mutation requires")
    print("  --execute --after-optical-gate --i-understand-device-mutation")


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


def execute(args: argparse.Namespace) -> int:
    # Intentionally conservative: Phase B execution is a later task. This path
    # snapshots counters only when the caller explicitly opted into mutation,
    # and it still refuses flash/reboot/service-restart here.
    print("Phase B execute is opt-in snapshot-only in this helper.", flush=True)
    print("It will not flash, reboot, inject, or send M4B3 frames.", flush=True)
    if args.flash or args.reboot or args.inject or args.host:
        print("flash/reboot/inject/host traffic is not implemented here.", flush=True)
        print("Use flash_app1_once.sh / m4b3_phase_d_run.py from a Phase B task.", flush=True)
        return 2
    st = m4adb("status")
    panel = m4adb("m4b3_panel")
    print(json.dumps({"status": st, "panel": panel}, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--print-checklist", action="store_true", help="print checklist and exit (default)")
    p.add_argument("--execute", action="store_true", help="opt-in later device path (requires both gates)")
    p.add_argument("--after-optical-gate", action="store_true", help="human confirmed #34 optical gate")
    p.add_argument(
        "--i-understand-device-mutation",
        action="store_true",
        help="acknowledge this can change M4/Android state",
    )
    p.add_argument("--flash", action="store_true", help="rejected: use flash_app1_once.sh in Phase B")
    p.add_argument("--reboot", action="store_true", help="rejected: reboot is a Phase B operator step")
    p.add_argument("--inject", action="store_true", help="rejected: use m4b3_phase_d_run.py in Phase B")
    p.add_argument("--host", help="rejected here; pass to m4b3_phase_d_run.py later")
    args = p.parse_args()

    if args.execute:
        if not args.after_optical_gate or not args.i_understand_device_mutation:
            print(
                "refusing execute: need --after-optical-gate and "
                "--i-understand-device-mutation (and #34 optical gate must be closed)",
                file=sys.stderr,
            )
            return 2
        return execute(args)

    print_checklist()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
