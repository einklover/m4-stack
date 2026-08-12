#!/usr/bin/env python3
"""Real Network Manager 1/2/3 journeys through the production Home UI.

This intentionally does not use the m4adb ``wifi_transfer`` shortcut because
that debug operation may auto-start a saved/QEMU STA connection and bypass the
real NetworkModeSelectionActivity. Each journey boots a fresh SD image, enters
File transfer from Home with physical key events, selects one production mode,
then proves the parent activity survives the child callback transition.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))

import m4sim  # noqa: E402


EXPECTED_PARENT = "CrossPointWebServer"
MODES = (
    ("1-phone-to-device", 0, "after web server start"),
    ("2-device-to-phone", 1, "after web server start"),
    ("3-calibre-wifi", 2, "Entering activity: CalibreConnect"),
)


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


def _json_call(pty: str, args: list[str], *, timeout: float = 12.0,
               retries: int = 16) -> dict[str, Any]:
    """Run one m4adb command and tolerate only transient synthetic-input busy."""
    last = ""
    for attempt in range(1, retries + 1):
        rc, last = m4sim.m4adb_once(pty, args, timeout=timeout)
        blob = m4sim._json_blob(last)
        if rc == 0 and isinstance(blob, dict):
            return blob
        if "busy" not in last.lower():
            break
        if attempt < retries:
            time.sleep(0.12)
    raise m4sim.M4SimError(
        f"m4adb {' '.join(args)} failed after {retries} attempt(s)\n{last[-1600:]}"
    )


def _status(pty: str) -> dict[str, Any]:
    return _json_call(pty, ["status"], timeout=10.0, retries=4)


def _send_key(pty: str, name: str) -> None:
    blob = _json_call(pty, ["key", name], timeout=10.0, retries=24)
    if blob.get("op") != "key":
        raise m4sim.M4SimError(f"unexpected key response for {name}: {blob}")


def _wait_activity(pty: str, proc: Any, qlog: Path, expected: str,
                   *, seconds: float) -> dict[str, Any]:
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            tail = qlog.read_text(encoding="utf-8", errors="replace")[-3000:] if qlog.is_file() else ""
            raise m4sim.M4SimError(
                f"QEMU exited while waiting for activity={expected} rc={proc.returncode}\n{tail}"
            )
        try:
            last = _status(pty)
        except m4sim.M4SimError:
            time.sleep(0.15)
            continue
        if last.get("activity") == expected:
            return last
        time.sleep(0.15)
    raise m4sim.M4SimError(
        f"activity never became {expected!r}; last={json.dumps(last, ensure_ascii=False)}"
    )


def _wait_log(proc: Any, qlog: Path, marker: str, *, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        text = qlog.read_text(encoding="utf-8", errors="replace") if qlog.is_file() else ""
        if marker in text:
            return
        time.sleep(0.15)
    tail = qlog.read_text(encoding="utf-8", errors="replace")[-4000:] if qlog.is_file() else ""
    raise m4sim.M4SimError(f"log marker not observed: {marker}\n--- qemu log tail ---\n{tail}")


def _assert_parent_stable(pty: str, proc: Any, qlog: Path, *, seconds: float) -> list[dict[str, Any]]:
    """Continuously prove the old callback-stack bounce does not recur."""
    deadline = time.monotonic() + seconds
    samples: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            tail = qlog.read_text(encoding="utf-8", errors="replace")[-3000:] if qlog.is_file() else ""
            raise m4sim.M4SimError(f"QEMU exited during stability window rc={proc.returncode}\n{tail}")
        st = _status(pty)
        samples.append(st)
        activity = st.get("activity")
        if activity != EXPECTED_PARENT:
            raise m4sim.M4SimError(
                "Network Manager bounced out of its parent after child callback: "
                f"activity={activity!r}, expected={EXPECTED_PARENT!r}"
            )
        time.sleep(0.18)
    return samples


def _enter_real_mode_selection(pty: str, proc: Any, qlog: Path) -> None:
    # Fresh SD + blank/default settings produce the canonical Home menu:
    # My Library, Recents, File transfer, Apps, Settings.
    _wait_activity(pty, proc, qlog, "Home", seconds=30.0)
    _send_key(pty, "down")
    _send_key(pty, "down")
    _send_key(pty, "confirm")
    _wait_activity(pty, proc, qlog, EXPECTED_PARENT, seconds=20.0)
    _wait_log(proc, qlog, "Entering activity: NetworkModeSelection", seconds=20.0)


def _run_mode(base: Path, qemu: Path, flash: Path, ready_seconds: float,
              label: str, down_count: int, expected_marker: str) -> dict[str, Any]:
    mode_root = base / label
    if mode_root.exists():
        shutil.rmtree(mode_root, ignore_errors=True)
    _set_session(mode_root)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)
    proc = None
    try:
        proc, pty, qlog = m4sim.boot_qemu(qemu, flash, sd, open_eth=True, psram_mb=8)
        ping = m4sim.wait_m4adb_ready(pty, proc, seconds=ready_seconds, qemu_log=qlog)
        _enter_real_mode_selection(pty, proc, qlog)

        for _ in range(down_count):
            _send_key(pty, "down")
        _send_key(pty, "confirm")

        # This proves the real child consumed Confirm and completed its
        # lifecycle transition; status alone still names the parent while the
        # mode picker remains open.
        _wait_log(proc, qlog, "Exiting activity: NetworkModeSelection", seconds=20.0)
        _wait_log(proc, qlog, expected_marker, seconds=30.0)

        first = _wait_activity(pty, proc, qlog, EXPECTED_PARENT, seconds=10.0)
        samples = _assert_parent_stable(pty, proc, qlog, seconds=3.0)
        final_ping = _json_call(pty, ["ping"], timeout=10.0, retries=4)
        if "protocol" not in final_ping or "firmware" not in final_ping:
            raise m4sim.M4SimError(f"post-transition ping incomplete: {final_ping}")

        result = {
            "mode": label,
            "ok": True,
            "activity": first.get("activity"),
            "wifi_connected": first.get("wifi_connected"),
            "wifi_ip": first.get("wifi_ip"),
            "stability_samples": len(samples),
            "protocol": final_ping.get("protocol"),
            "firmware": final_ping.get("firmware"),
            "boot_ping": ping,
        }
        (mode_root / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(f"NETWORK MODE PASS: {label} samples={len(samples)}", flush=True)
        return result
    finally:
        _terminate(proc)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="m4sim test network-manager",
        description="Boot three fresh real-UI Network Manager journeys.",
    )
    p.add_argument("image", nargs="?", help="optional debug-capable firmware.bin or 16MiB flash image")
    p.add_argument("--plugin-debug", action="store_true", default=True,
                   help="use murphy_m4_qemu_plugin (default)")
    p.add_argument("--build-dir")
    p.add_argument("--skip-build", action="store_true",
                   help="reuse an existing murphy_m4_qemu_plugin PIO build")
    p.add_argument("--qemu", help="explicit patched v3 qemu-system-xtensa")
    p.add_argument("--ready-seconds", type=float, default=120.0)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    base = Path(os.environ.get("M4SIM_NETWORK_TMP", "/tmp/m4sim-network-manager"))
    if base.exists():
        shutil.rmtree(base, ignore_errors=True)
    base.mkdir(parents=True, exist_ok=True)

    try:
        qemu = m4sim.find_qemu(args.qemu, build_if_missing=True)
        build_root = base / "_build"
        _set_session(build_root)
        ns = argparse.Namespace(
            image=args.image,
            plugin_debug=bool(args.plugin_debug or not args.image),
            build_dir=args.build_dir,
            skip_build=bool(args.skip_build),
        )
        flash = m4sim.resolve_flash(ns)

        results = []
        for label, down_count, marker in MODES:
            results.append(_run_mode(base, qemu, flash, args.ready_seconds, label, down_count, marker))

        summary = {"ok": True, "modes": results}
        (base / "network-manager-summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(f"NETWORK MANAGER PASS: {len(results)}/3", flush=True)
        print(f"artifacts: {base}", flush=True)
        return 0
    except m4sim.M4SimError as exc:
        print(f"NETWORK MANAGER FAIL: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
