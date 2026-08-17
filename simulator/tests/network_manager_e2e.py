#!/usr/bin/env python3
"""Real Network Manager 1/2/3 journeys through the production Home UI.

This intentionally does not use the m4adb ``wifi_transfer`` shortcut because
that debug operation may auto-start a saved/QEMU STA connection and bypass the
real NetworkModeSelectionActivity. Each journey boots a fresh SD image, enters
File transfer from Home with physical key events, selects one production mode,
then proves the parent and expected child activity remain stable.

The top-level status reports CrossPointWebServer while its nested activity is
active. Use the existing structured ``ui`` dump to observe that child instead
of relying on ordinary Serial lifecycle text, which is not part of the m4adb
control-plane log in every QEMU/CI configuration.
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
from typing import Any, Callable, TypeVar

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))

import m4sim  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402
from m4adb_lib.transport import SerialTransport  # noqa: E402
from m4adb_observing_client import ObservingClient as Client  # noqa: E402


EXPECTED_PARENT = "CrossPointWebServer"
MODES = (
    ("1-phone-to-device", 0, ""),
    ("2-device-to-phone", 1, ""),
    ("3-calibre-wifi", 2, "CalibreConnect"),
)
T = TypeVar("T")


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


def _call_busy_retry(fn: Callable[[], T], *, retries: int = 16) -> T:
    """Retry only the synthetic-input busy response; preserve real failures."""
    last: BridgeError | None = None
    for attempt in range(1, retries + 1):
        try:
            return fn()
        except BridgeError as exc:
            last = exc
            if exc.key != "busy" or attempt >= retries:
                raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
            time.sleep(0.12)
    raise m4sim.M4SimError(f"m4adb busy after {retries} attempts: {last}")


def _status(client: Client) -> dict[str, Any]:
    result = _call_busy_retry(client.status, retries=4)
    if not isinstance(result, dict):
        raise m4sim.M4SimError(f"unexpected status response: {result!r}")
    return result


def _ui(client: Client) -> dict[str, Any]:
    result = _call_busy_retry(client.ui, retries=4)
    if not isinstance(result, dict):
        raise m4sim.M4SimError(f"unexpected ui response: {result!r}")
    return result


def _subactivity(ui: dict[str, Any]) -> str:
    outer = ui.get("ui")
    if not isinstance(outer, dict):
        return ""
    body = outer.get("body")
    if not isinstance(body, dict):
        return ""
    value = body.get("subactivity")
    return value if isinstance(value, str) else ""


def _send_key(client: Client, name: str) -> None:
    # A physical key can legitimately arrive while the previous e-ink refresh
    # still owns the input bridge for a few seconds. Wait for that bounded
    # backpressure instead of tying correctness to a fixed retry count.
    deadline = time.monotonic() + 8.0
    while True:
        try:
            blob = client.key(name)
            break
        except BridgeError as exc:
            if exc.key != "busy":
                raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
            if time.monotonic() >= deadline:
                raise m4sim.M4SimError(
                    f"m4adb busy: input bridge stayed busy while sending {name!r}"
                ) from exc
            time.sleep(0.12)
    if not isinstance(blob, dict) or blob.get("op") != "key":
        raise m4sim.M4SimError(f"unexpected key response for {name}: {blob}")


def _qemu_tail(qlog: Path, chars: int = 3000) -> str:
    return qlog.read_text(encoding="utf-8", errors="replace")[-chars:] if qlog.is_file() else ""


def _serial_text(client: Client) -> str:
    return "\n".join(client.serial_log)


def _save_serial(client: Client | None, path: Path) -> None:
    if client is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    text = _serial_text(client)
    path.write_text(text + ("\n" if text else ""), encoding="utf-8")


def _wait_activity(client: Client, proc: Any, qlog: Path, expected: str,
                   *, seconds: float) -> dict[str, Any]:
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(
                f"QEMU exited while waiting for activity={expected} rc={proc.returncode}\n{_qemu_tail(qlog)}"
            )
        try:
            last = _status(client)
        except m4sim.M4SimError:
            time.sleep(0.15)
            continue
        if last.get("activity") == expected:
            return last
        time.sleep(0.15)
    raise m4sim.M4SimError(
        f"activity never became {expected!r}; last={json.dumps(last, ensure_ascii=False)}"
    )


def _wait_subactivity(client: Client, proc: Any, qlog: Path, expected: str,
                      *, seconds: float) -> dict[str, Any]:
    """Wait for the real nested Network Manager child via structured UI state."""
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(
                f"QEMU exited while waiting for subactivity={expected!r} rc={proc.returncode}\n{_qemu_tail(qlog)}"
            )
        try:
            last = _ui(client)
        except m4sim.M4SimError:
            time.sleep(0.15)
            continue
        if last.get("activity") == EXPECTED_PARENT and _subactivity(last) == expected:
            return last
        time.sleep(0.15)
    raise m4sim.M4SimError(
        f"subactivity never became {expected!r}; last={json.dumps(last, ensure_ascii=False)}"
    )


def _assert_mode_stable(client: Client, proc: Any, qlog: Path, expected_subactivity: str,
                        *, seconds: float) -> list[dict[str, Any]]:
    """Continuously prove the old mode-1/mode-3 callback bounce does not recur."""
    deadline = time.monotonic() + seconds
    samples: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(
                f"QEMU exited during stability window rc={proc.returncode}\n{_qemu_tail(qlog)}"
            )
        st = _status(client)
        ui = _ui(client)
        sample = {
            "activity": st.get("activity"),
            "subactivity": _subactivity(ui),
        }
        samples.append(sample)
        if sample["activity"] != EXPECTED_PARENT or sample["subactivity"] != expected_subactivity:
            raise m4sim.M4SimError(
                "Network Manager mode bounced after child callback: "
                f"activity={sample['activity']!r}, subactivity={sample['subactivity']!r}, "
                f"expected=({EXPECTED_PARENT!r}, {expected_subactivity!r})"
            )
        time.sleep(0.18)
    return samples


def _enter_real_mode_selection(client: Client, proc: Any, qlog: Path) -> None:
    # Fresh SD + pristine flash/default settings produce the canonical Home menu:
    # My Library, Recents, File transfer, Apps, Settings.
    _wait_activity(client, proc, qlog, "Home", seconds=30.0)
    _send_key(client, "down")
    _send_key(client, "down")
    _send_key(client, "confirm")
    # The picker is a nested child; top-level status intentionally remains the
    # CrossPointWebServer parent. Structured UI state is the truthful signal.
    _wait_subactivity(client, proc, qlog, "NetworkModeSelection", seconds=20.0)


def _run_mode(base: Path, qemu: Path, flash: Path, ready_seconds: float,
              label: str, down_count: int, expected_subactivity: str) -> dict[str, Any]:
    mode_root = base / label
    if mode_root.exists():
        shutil.rmtree(mode_root, ignore_errors=True)
    _set_session(mode_root)
    # QEMU's MTD drive is writable. Give every journey a pristine copy so NVS
    # or other flash writes from mode 1 cannot change Home/settings for mode 2/3.
    mode_flash = mode_root / "flash-16m.bin"
    mode_flash.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(flash, mode_flash)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)
    proc = None
    client: Client | None = None
    try:
        proc, pty, qlog = m4sim.boot_qemu(qemu, mode_flash, sd, open_eth=True, psram_mb=8)
        # First prove the bridge is up using the same public readiness path as
        # m4sim smoke, then keep one direct client open for the whole journey.
        ping = m4sim.wait_m4adb_ready(pty, proc, seconds=ready_seconds, qemu_log=qlog)
        client = Client(SerialTransport(pty), default_timeout=10.0)
        client.wait_ready(timeout=min(20.0, max(5.0, ready_seconds)))
        _enter_real_mode_selection(client, proc, qlog)

        for _ in range(down_count):
            _send_key(client, "down")
        _send_key(client, "confirm")

        # Prove the picker consumed Confirm and reached the expected production
        # child state. Modes 1/2 release the picker; mode 3 replaces it with
        # CalibreConnect. This also detects the historical immediate bounce.
        _wait_subactivity(client, proc, qlog, expected_subactivity, seconds=30.0)

        first = _wait_activity(client, proc, qlog, EXPECTED_PARENT, seconds=10.0)
        samples = _assert_mode_stable(client, proc, qlog, expected_subactivity, seconds=3.0)
        final_ping = _call_busy_retry(client.ping, retries=4)
        if not isinstance(final_ping, dict) or "protocol" not in final_ping or "firmware" not in final_ping:
            raise m4sim.M4SimError(f"post-transition ping incomplete: {final_ping}")

        result = {
            "mode": label,
            "ok": True,
            "activity": first.get("activity"),
            "subactivity": expected_subactivity,
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
        print(
            f"NETWORK MODE PASS: {label} child={expected_subactivity or '<none>'} samples={len(samples)}",
            flush=True,
        )
        return result
    except BridgeError as exc:
        raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
    finally:
        _save_serial(client, mode_root / "artifacts" / "firmware-serial.log")
        if client is not None:
            client.close()
        _terminate(proc)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="m4sim test network-manager",
        description="Boot three fresh real-UI Network Manager journeys.",
    )
    p.add_argument("image", nargs="?", help="optional debug-capable firmware.bin or 16MiB flash image")
    p.add_argument("--plugin-debug", action="store_true",
                   help="force murphy_m4_qemu_plugin even when an image path is supplied")
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
        for label, down_count, expected_subactivity in MODES:
            results.append(
                _run_mode(base, qemu, flash, args.ready_seconds, label, down_count, expected_subactivity)
            )

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
