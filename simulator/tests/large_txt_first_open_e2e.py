#!/usr/bin/env python3
"""Measure first-readable/first-physical open of a large local TXT book.

The optimized run must enter the native TXT reader, expose a first page before
the progressive index is complete, and use zero legacy FULL/HALF waveforms.
``--baseline`` runs the same fixture on a pre-optimization flash
image and records the legacy TXT->EPUB conversion path for before/after data.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
from typing import Any

from serial.serialutil import SerialTimeoutException

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))
sys.path.insert(0, str(ROOT / "simulator" / "tests"))

import m4sim  # noqa: E402
import reader_ui_e2e as reader_ui  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402
from m4adb_lib.transport import make_transport  # noqa: E402
from m4adb_observing_client import ObservingClient as Client  # noqa: E402

FIXTURE_NAME = "large-open.txt"
HIGH_DENSITY_FIXTURE_NAME = "large-high-density.txt"
HIGH_DENSITY_CHAPTERS = 40416
HIGH_DENSITY_RECORD_BYTES = 130
LEGACY_STRONG_EFF = {0, 1}  # Legacy GfxRenderer FULL/HALF; must be normalized away.
READER_CLEANUP_EFF = 4
GFX_RE = re.compile(r"\[GFX\].*displayBuffer mode=(\d+) eff=(\d+)")


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


def _qlog_lines(qlog: Path) -> list[str]:
    if not qlog.is_file():
        return []
    return qlog.read_text(encoding="utf-8", errors="replace").splitlines()


def _log_lines(client: Client, qlog: Path) -> list[str]:
    # QEMU's PTY backend keeps firmware serial off qemu.log; ObservingClient
    # mirrors those lines in serial_log. Pipe backends still work through the
    # qemu-log fallback when no serial lines have arrived yet.
    serial = list(client.serial_log)
    return serial if serial else _qlog_lines(qlog)


def _marker(lines: list[str], needle: str, start: int = 0) -> tuple[int, str] | None:
    for i in range(start, len(lines)):
        if needle in lines[i]:
            return i, lines[i]
    return None


def _drain_serial(client: Client, timeout: float = 0.1) -> None:
    """Keep observing firmware output when a busy guest cannot accept a poll."""
    data = client.t.read(timeout=timeout)
    if not data:
        return
    for raw_line, frame in client.parser.feed(data):
        client._emit_log(raw_line)


def _fw_time(line: str) -> int | None:
    match = re.search(r"\bt=(\d+)", line)
    if match:
        return int(match.group(1))
    match = re.match(r"\[(\d+)\]", line)
    return int(match.group(1)) if match else None


def _seed_large_txt(sd: Path, root: Path, fixture_mb: int, *, high_chapter_density: bool = False) -> Path:
    mcopy = shutil.which("mcopy")
    mmd = shutil.which("mmd")
    if not (mcopy and mmd):
        raise m4sim.M4SimError("mtools (mcopy/mmd) required for large-TXT measurement")

    for directory in ("::/.crosspoint",):
        subprocess.run([mmd, "-i", str(sd), directory], capture_output=True)

    fixture_name = HIGH_DENSITY_FIXTURE_NAME if high_chapter_density else FIXTURE_NAME
    fixture = root / fixture_name
    if high_chapter_density:
        # Fixed-size records make the pathological case deterministic: every
        # record starts a short chapter, so the 5 MiB / 40,416-chapter input
        # produces roughly 1,617 cached 25-title batches.
        with fixture.open("wb") as out:
            for chapter in range(1, HIGH_DENSITY_CHAPTERS + 1):
                prefix = f"第{chapter:05d}章\n短章内容。\n".encode("utf-8")
                if len(prefix) >= HIGH_DENSITY_RECORD_BYTES:
                    raise m4sim.M4SimError("high-density chapter prefix exceeded fixed record size")
                out.write(prefix)
                out.write(b"x" * (HIGH_DENSITY_RECORD_BYTES - len(prefix)))
    else:
        target = fixture_mb * 1024 * 1024
        block = (
            "第0001章\n"
            "This large TXT fixture keeps chapter-like boundaries while making the "
            "first page cheap to read. The rest must remain lazy and progressive.\n"
            "A short paragraph is repeated to make conversion/indexing measurable.\n\n"
        ).encode("utf-8")
        with fixture.open("wb") as out:
            while out.tell() < target:
                out.write(block[: target - out.tell()])

    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(fixture), f"::/{fixture_name}"],
        capture_output=True,
        text=True,
    )
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy TXT fixture failed: {cp.stdout}{cp.stderr}")

    settings = root / "settings.json"
    # Direct TXT is deliberately off so the optimized firmware must prove the
    # large-file auto-direct policy instead of inheriting the user preference.
    settings.write_text('{"v":1,"directTxtRead":0,"libraryLongPressMenu":1}\n', encoding="utf-8")
    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(settings), "::/.crosspoint/settings.json"],
        capture_output=True,
        text=True,
    )
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy settings failed: {cp.stdout}{cp.stderr}")
    return fixture


def _wait_open_measurement(
    client: Client,
    proc: Any,
    qlog: Path,
    *,
    baseline: bool,
    open_at: float,
    open_log_index: int,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    epub_seen_at: float | None = None
    readable_at: float | None = None
    physical_at: float | None = None
    readable_idx: int | None = None
    physical_idx: int | None = None
    start_idx = open_log_index
    epub_idx: int | None = None

    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(
                f"QEMU exited during large-TXT open (rc={proc.returncode})\n"
                + reader_ui._qemu_tail(qlog, 5000)
            )
        # There is no background serial reader in m4adb; a short UI request
        # drains firmware lines that arrived between journey actions.
        try:
            live_ui = client.request({"op": "ui"}, timeout=0.5)
        except (BridgeError, SerialTimeoutException):
            live_ui = {}
            _drain_serial(client)
        lines = _log_lines(client, qlog)
        if baseline:
            if epub_seen_at is None:
                path = reader_ui._activity_path(live_ui) if live_ui else ()
                if path[:2] == ("Reader", "EpubReader"):
                    epub_seen_at = time.monotonic()
                    found = _marker(lines, "Entering activity: EpubReader", open_log_index)
                    epub_idx = found[0] if found else len(lines)
            if epub_seen_at is not None and physical_idx is None:
                scan_start = (epub_idx + 1) if epub_idx is not None else open_log_index
                for i in range(scan_start, len(lines)):
                    if GFX_RE.search(lines[i]):
                        physical_idx = i
                        physical_at = time.monotonic()
                        readable_idx = i
                        readable_at = physical_at
                        break
        else:
            if readable_idx is None:
                found = _marker(lines, "first_page_ready kind=library", open_log_index)
                if found:
                    readable_idx, _ = found
                    readable_at = time.monotonic()
            if physical_idx is None:
                found = _marker(lines, "first_physical_done kind=library", open_log_index)
                if found:
                    physical_idx, _ = found
                    physical_at = time.monotonic()
        if readable_at is not None and physical_at is not None:
            return {
                "readable_at": readable_at,
                "physical_at": physical_at,
                "readable_idx": readable_idx,
                "physical_idx": physical_idx,
                "start_idx": start_idx,
                "epub_seen_at": epub_seen_at,
                "lines": lines,
            }
        time.sleep(0.15)

    raise m4sim.M4SimError(
        f"large-TXT open did not reach readable={readable_at is not None} "
        f"physical={physical_at is not None} within {timeout:.0f}s\n"
        + reader_ui._qemu_tail(qlog, 7000)
    )


def _refresh_stats(lines: list[str], start: int, end: int) -> dict[str, Any]:
    calls: list[dict[str, Any]] = []
    for line in lines[start : end + 1]:
        match = GFX_RE.search(line)
        if not match:
            continue
        calls.append({"mode": int(match.group(1)), "effective": int(match.group(2)), "line": line})
    obvious = [call for call in calls if call["effective"] in LEGACY_STRONG_EFF]
    visible_inversion_phases = sum(
        2 if call["effective"] == 0 else 1
        for call in calls
        if call["effective"] in LEGACY_STRONG_EFF or call["effective"] == READER_CLEANUP_EFF
    )
    full_waveform_phases = sum(2 for call in calls if call["effective"] == 0)
    return {
        "display_buffer_calls": len(calls),
        "obvious_full_or_half_calls": len(obvious),
        "visible_inversion_phases": visible_inversion_phases,
        "full_waveform_phases": full_waveform_phases,
        "calls": calls,
    }


def _run(base: Path, qemu: Path, flash: Path, *, baseline: bool, fixture_mb: int,
         high_chapter_density: bool, ready_seconds: float, open_timeout: float) -> dict[str, Any]:
    label = "before" if baseline else "after"
    root = base / label
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    _set_session(root)
    root.mkdir(parents=True, exist_ok=True)

    journey_flash = root / "flash-16m.bin"
    shutil.copy2(flash, journey_flash)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)
    fixture = _seed_large_txt(sd, root, fixture_mb, high_chapter_density=high_chapter_density)

    proc = None
    client: Client | None = None
    try:
        proc, pty, qlog = m4sim.boot_qemu(qemu, journey_flash, sd, open_eth=True, psram_mb=8)
        m4sim.wait_m4adb_ready(pty, proc, seconds=ready_seconds, qemu_log=qlog)
        client = Client(make_transport(pty), default_timeout=10.0)
        client.wait_ready(timeout=min(20.0, max(5.0, ready_seconds)))

        reader_ui._wait_top(client, proc, qlog, "Home", seconds=45.0)
        reader_ui._send_key(client, "confirm")
        reader_ui._wait_top(client, proc, qlog, "MyLibrary", seconds=30.0)
        reader_ui._select_library_entry(client, proc, qlog, fixture.name)

        open_log_index = len(_log_lines(client, qlog))
        open_at = time.monotonic()
        reader_ui._send_key(client, "confirm")
        measured = _wait_open_measurement(
            client, proc, qlog, baseline=baseline, open_at=open_at,
            open_log_index=open_log_index, timeout=open_timeout,
        )
        lines = measured["lines"]
        readable_idx = int(measured["readable_idx"])
        physical_idx = int(measured["physical_idx"])
        refresh = _refresh_stats(lines, int(measured["start_idx"]), physical_idx)
        chapter_before_physical = _marker(lines, "large_txt_chapter_batch", int(measured["start_idx"]))

        chapter_seen = False
        chapter_seen_at: float | None = None
        if not baseline:
            chapter_deadline = time.monotonic() + 25.0
            while time.monotonic() < chapter_deadline:
                try:
                    client.request({"op": "ui"}, timeout=0.5)
                except (BridgeError, SerialTimeoutException):
                    _drain_serial(client)
                lines = _log_lines(client, qlog)
                found = _marker(lines, "large_txt_chapter_batch", physical_idx)
                if found:
                    chapter_seen = True
                    chapter_seen_at = time.monotonic()
                    break
                if proc.poll() is not None:
                    break
                time.sleep(0.25)

        first_ready_line = lines[readable_idx] if readable_idx < len(lines) else ""
        first_physical_line = lines[physical_idx] if physical_idx < len(lines) else ""
        open_fw_line = _marker(lines, "large_txt_auto_direct", open_log_index)
        if open_fw_line is None:
            open_fw_line = _marker(lines, "Entering activity: Reader", open_log_index)
        open_fw_ms = _fw_time(open_fw_line[1]) if open_fw_line else None
        readable_fw_ms = _fw_time(first_ready_line)
        physical_fw_ms = _fw_time(first_physical_line)
        result = {
            "ok": True,
            "label": label,
            "baseline": baseline,
            "fixture_bytes": fixture.stat().st_size,
            "fixture": fixture.name,
            "high_chapter_density": high_chapter_density,
            "first_readable_seconds_host": measured["readable_at"] - open_at,
            "first_physical_seconds_host": measured["physical_at"] - open_at,
            "first_readable_firmware_line": first_ready_line,
            "first_physical_firmware_line": first_physical_line,
            "open_firmware_ms": open_fw_ms,
            "first_readable_firmware_ms": readable_fw_ms,
            "first_physical_firmware_ms": physical_fw_ms,
            "first_readable_firmware_delta_ms":
                (readable_fw_ms - open_fw_ms) if readable_fw_ms is not None and open_fw_ms is not None else None,
            "first_physical_firmware_delta_ms":
                (physical_fw_ms - open_fw_ms) if physical_fw_ms is not None and open_fw_ms is not None else None,
            "refresh": refresh,
            "chapter_discovery_before_first_physical": bool(
                chapter_before_physical is not None and chapter_before_physical[0] <= physical_idx
            ),
            "chapter_batch_seen_after_first_physical": chapter_seen,
            "chapter_batch_seconds_after_first_physical":
                (chapter_seen_at - measured["physical_at"]) if chapter_seen_at is not None else None,
            "conversion_before_first_physical": any(
                "txt_to_epub" in line.lower() or "starting txt" in line.lower()
                for line in lines[int(measured["start_idx"]): physical_idx + 1]
            ),
            "optimized_direct_marker": _marker(lines, "large_txt_auto_direct", open_log_index) is not None,
            "first_page_index_incomplete": "index_complete=0" in first_ready_line,
        }
        (root / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)

        if not baseline:
            if not result["optimized_direct_marker"]:
                raise m4sim.M4SimError("optimized run never logged large_txt_auto_direct")
            if result["conversion_before_first_physical"]:
                raise m4sim.M4SimError("TXT->EPUB conversion appeared before optimized first physical page")
            if not result["first_page_index_incomplete"]:
                raise m4sim.M4SimError("first page was not observed before index completion")
            if refresh["obvious_full_or_half_calls"] != 0:
                raise m4sim.M4SimError(
                    f"legacy full/half refresh escaped policy: {refresh['obvious_full_or_half_calls']}"
                )
            if refresh["full_waveform_phases"] != 0:
                raise m4sim.M4SimError("legacy full waveform appeared before first physical page")
            if refresh["visible_inversion_phases"] != 0:
                raise m4sim.M4SimError(
                    "visible inversion phase appeared outside reader-body cleanup before first physical page"
                )
            if high_chapter_density:
                if result["first_physical_seconds_host"] > 3.0:
                    raise m4sim.M4SimError(
                        f"high-density first physical page exceeded 3s: {result['first_physical_seconds_host']:.3f}s"
                    )
                if result["chapter_discovery_before_first_physical"]:
                    raise m4sim.M4SimError("chapter discovery ran before high-density first physical page")
            if not chapter_seen:
                raise m4sim.M4SimError("chapter discovery did not continue in the background")
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
    p = argparse.ArgumentParser(prog="m4sim test large-txt")
    p.add_argument("--baseline", action="store_true", help="measure a pre-optimization flash image")
    p.add_argument("--flash", help="explicit 16MiB flash image; skips the current PIO build")
    p.add_argument("--build-dir", help="PIO build dir when --flash is an app-only firmware.bin")
    p.add_argument("--fixture-mb", type=int, default=12)
    p.add_argument("--high-chapter-density", action="store_true",
                   help="use the deterministic 5 MiB / 40,416-chapter fixture")
    p.add_argument("--open-timeout", type=float, default=300.0)
    p.add_argument("--qemu")
    p.add_argument("--ready-seconds", type=float, default=150.0)
    p.add_argument("--skip-build", action="store_true")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    base = Path(os.environ.get("M4SIM_LARGE_TXT_TMP", "/tmp/m4sim-large-txt"))
    base.mkdir(parents=True, exist_ok=True)
    try:
        qemu = m4sim.find_qemu(args.qemu, build_if_missing=True)
        build_root = base / "_build"
        _set_session(build_root)
        ns = argparse.Namespace(
            image=args.flash,
            plugin_debug=not bool(args.flash),
            build_dir=args.build_dir,
            skip_build=bool(args.skip_build),
        )
        flash = m4sim.resolve_flash(ns)
        result = _run(
            base, qemu, flash, baseline=bool(args.baseline), fixture_mb=max(5, args.fixture_mb),
            high_chapter_density=bool(args.high_chapter_density),
            ready_seconds=args.ready_seconds, open_timeout=args.open_timeout,
        )
        (base / f"large-txt-{result['label']}-summary.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(
            f"LARGE TXT {result['label'].upper()} PASS: "
            f"readable={result['first_readable_seconds_host']:.3f}s "
            f"physical={result['first_physical_seconds_host']:.3f}s "
            f"obvious_refreshes={result['refresh']['obvious_full_or_half_calls']}",
            flush=True,
        )
        return 0
    except m4sim.M4SimError as exc:
        print(f"LARGE TXT {'BEFORE' if args.baseline else 'AFTER'} FAIL: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
