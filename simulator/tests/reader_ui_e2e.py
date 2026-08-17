#!/usr/bin/env python3
"""Real local-TXT reader UI journey through production Home/MyLibrary.

This is intentionally a full application journey, not a menu unit test:
Home -> MyLibrary -> Reader -> TxtReader -> Quick -> Progress -> Style -> More
-> TxtReader. It drives production key/touch input through m4adb and captures
real SSD1677 framebuffer screenshots from patched QEMU.

The SD fixture is injected into the raw FAT32 image with mtools before boot, so
no debug-only firmware file-open shortcut is involved.
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
from typing import Any, Callable, TypeVar

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "simulator"))
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))

import m4sim  # noqa: E402
from m4adb_lib.client import BridgeError  # noqa: E402
from m4adb_lib.transport import SerialTransport  # noqa: E402
from m4adb_observing_client import ObservingClient as Client  # noqa: E402

T = TypeVar("T")
READER_PATH = ("Reader", "TxtReader")
MENU_PATH = ("Reader", "TxtReader", "EpubReaderMenu")
PROGRESS_PATH = ("Reader", "TxtReader", "EpubReaderPercentSelection")


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


def _call_busy_retry(fn: Callable[[], T], *, seconds: float = 8.0) -> T:
    deadline = time.monotonic() + seconds
    while True:
        try:
            return fn()
        except BridgeError as exc:
            if exc.key != "busy" or time.monotonic() >= deadline:
                raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
            time.sleep(0.12)


def _send_key(client: Client, name: str) -> None:
    result = _call_busy_retry(lambda: client.key(name))
    if not isinstance(result, dict) or result.get("op") != "key":
        raise m4sim.M4SimError(f"unexpected key response for {name}: {result}")


def _tap(client: Client, x: int, y: int) -> None:
    result = _call_busy_retry(lambda: client.tap(x, y))
    if not isinstance(result, dict) or result.get("op") != "tap":
        raise m4sim.M4SimError(f"unexpected tap response at ({x},{y}): {result}")


def _ui(client: Client) -> dict[str, Any]:
    result = _call_busy_retry(client.ui)
    if not isinstance(result, dict):
        raise m4sim.M4SimError(f"unexpected ui response: {result!r}")
    return result


def _activity_path(ui: dict[str, Any]) -> tuple[str, ...]:
    path: list[str] = []
    top = ui.get("activity")
    if isinstance(top, str) and top:
        path.append(top)

    outer = ui.get("ui")
    body: Any = outer.get("body") if isinstance(outer, dict) else None
    # ActivityWithSubactivity::debugUiJson() recursively exposes
    # {subactivity, child}. Stop defensively on malformed/truncated dumps.
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


def _qemu_tail(qlog: Path, chars: int = 3000) -> str:
    return qlog.read_text(encoding="utf-8", errors="replace")[-chars:] if qlog.is_file() else ""


def _wait_path(client: Client, proc: Any, qlog: Path, expected: tuple[str, ...], *, seconds: float) -> dict[str, Any]:
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    last_path: tuple[str, ...] = ()
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(
                f"QEMU exited while waiting for path={expected!r} rc={proc.returncode}\n{_qemu_tail(qlog)}"
            )
        try:
            last = _ui(client)
            last_path = _activity_path(last)
        except m4sim.M4SimError:
            time.sleep(0.15)
            continue
        if last_path[: len(expected)] == expected:
            return last
        time.sleep(0.15)
    raise m4sim.M4SimError(
        f"activity path never became {expected!r}; last_path={last_path!r}; "
        f"last={json.dumps(last, ensure_ascii=False)}"
    )


def _wait_top(client: Client, proc: Any, qlog: Path, expected: str, *, seconds: float) -> dict[str, Any]:
    return _wait_path(client, proc, qlog, (expected,), seconds=seconds)


def _capture(client: Client, root: Path, name: str) -> Path:
    out = root / "artifacts" / f"{name}.pbm"
    out.parent.mkdir(parents=True, exist_ok=True)
    # Allow the display task to submit the requested e-ink refresh before the
    # bridge snapshots the framebuffer. This is synchronization, not UI logic.
    time.sleep(0.35)
    _call_busy_retry(lambda: client.screenshot(out), seconds=12.0)
    if not out.is_file() or out.stat().st_size <= 32:
        raise m4sim.M4SimError(f"missing/empty screenshot: {out}")
    return out


def _assert_changed(a: Path, b: Path, label: str) -> None:
    if a.read_bytes() == b.read_bytes():
        raise m4sim.M4SimError(f"framebuffer did not change for {label}: {a.name} == {b.name}")


def _seed_txt(sd: Path, root: Path) -> Path:
    mcopy = shutil.which("mcopy")
    if not mcopy:
        raise m4sim.M4SimError("mcopy not found; install mtools for reader-ui SD fixture injection")
    fixture = root / "reader-ui-fixture.txt"
    lines: list[str] = [
        "墨水屏阅读器 QEMU 触控界面回归测试",
        "这是一份仅用于模拟器的 UTF-8 本地 TXT。",
        "目录、进度、排版、书签与更多菜单必须在同一真实阅读器路径中工作。",
        "",
    ]
    for i in range(1, 241):
        lines.append(
            f"第{i:03d}段  天地玄黄，宇宙洪荒。阅读体验应保持清晰、克制、稳定；"
            "触控操作采用离散提交，避免墨水屏连续重绘。"
        )
    fixture.write_text("\n".join(lines) + "\n", encoding="utf-8")
    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(fixture), "::/reader-ui-fixture.txt"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if cp.returncode != 0:
        raise m4sim.M4SimError(f"mcopy fixture failed ({cp.returncode}):\n{cp.stdout}")
    return fixture


def _save_serial(client: Client | None, path: Path) -> None:
    if client is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(client.serial_log)
    path.write_text(text + ("\n" if text else ""), encoding="utf-8")


def _run(base: Path, qemu: Path, flash: Path, ready_seconds: float) -> dict[str, Any]:
    root = base / "txt-touch-journey"
    if root.exists():
        shutil.rmtree(root, ignore_errors=True)
    _set_session(root)
    root.mkdir(parents=True, exist_ok=True)

    # QEMU MTD is writable. Keep this journey isolated from smoke/network NVS.
    journey_flash = root / "flash-16m.bin"
    shutil.copy2(flash, journey_flash)
    sd = m4sim.ensure_sd(fresh=True, size_mb=64)
    fixture = _seed_txt(sd, root)

    proc = None
    client: Client | None = None
    try:
        proc, pty, qlog = m4sim.boot_qemu(qemu, journey_flash, sd, open_eth=True, psram_mb=8)
        boot_ping = m4sim.wait_m4adb_ready(pty, proc, seconds=ready_seconds, qemu_log=qlog)
        client = Client(SerialTransport(pty), default_timeout=10.0)
        client.wait_ready(timeout=min(20.0, max(5.0, ready_seconds)))

        # 1. Real production Home -> MyLibrary -> direct TXT reader.
        _wait_top(client, proc, qlog, "Home", seconds=30.0)
        home = _capture(client, root, "01-home")
        _send_key(client, "confirm")
        _wait_top(client, proc, qlog, "MyLibrary", seconds=20.0)
        library = _capture(client, root, "02-library")
        _send_key(client, "confirm")
        reader_ui = _wait_path(client, proc, qlog, READER_PATH, seconds=60.0)
        reader = _capture(client, root, "03-reader")

        # 2. Open Quick with the real reader Confirm action.
        _send_key(client, "confirm")
        quick_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        quick = _capture(client, root, "04-quick")

        # Portrait quick geometry: action #1 (Progress) center ~= (240,136).
        _tap(client, 240, 136)
        progress_ui = _wait_path(client, proc, qlog, PROGRESS_PATH, seconds=20.0)
        progress = _capture(client, root, "05-progress")

        # Progress track is x=24..455, y=134..143 in portrait. One tap near 75%
        # validates the enlarged one-shot touch target without drag/repaint churn.
        _tap(client, 347, 139)
        progress_seek = _capture(client, root, "06-progress-seek")
        _assert_changed(progress, progress_seek, "progress seek")
        _send_key(client, "back")
        _wait_path(client, proc, qlog, READER_PATH, seconds=20.0)

        # 3. Re-open Quick and enter Style using the Quick tile itself.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 390, 136)  # action #2: typography/style
        # Style is a layer of EpubReaderMenu, so path is unchanged; framebuffer
        # change proves the layer transition while the same Activity remains.
        time.sleep(0.2)
        style = _capture(client, root, "07-style")
        _assert_changed(quick, style, "quick -> style")

        # Connected A- segment center ~= (93,113). Default system font is 18px;
        # decreasing it exercises style dirty/reflow notification deterministically.
        _tap(client, 93, 113)
        style_adjusted = _capture(client, root, "08-style-font-minus")
        _assert_changed(style, style_adjusted, "font size decrease")

        # Back from Style returns to Quick, not to the book.
        _send_key(client, "back")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        quick_after_style = _capture(client, root, "09-quick-after-style")

        # Quick action #5 More is bottom-right center ~= (390,236).
        _tap(client, 390, 236)
        time.sleep(0.2)
        more = _capture(client, root, "10-more")
        _assert_changed(quick_after_style, more, "quick -> more")

        # Back: More -> Quick; Back again: menu -> reader and applies changed style.
        _send_key(client, "back")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _send_key(client, "back")
        final_ui = _wait_path(client, proc, qlog, READER_PATH, seconds=40.0)
        reader_after = _capture(client, root, "11-reader-after-style")

        # Distinct major surfaces are a useful framebuffer sanity check, but
        # activity-path assertions above remain the primary correctness signal.
        _assert_changed(home, library, "home -> library")
        _assert_changed(library, reader, "library -> reader")
        _assert_changed(reader, quick, "reader -> quick")
        _assert_changed(quick, progress, "quick -> progress")
        _assert_changed(more, reader_after, "more -> reader")

        final_ping = _call_busy_retry(client.ping)
        serial = "\n".join(client.serial_log)
        required_logs = (
            "Entering activity: TxtReader",
            "Entering activity: EpubReaderMenu",
            "Entering activity: EpubReaderPercentSelection",
        )
        missing_logs = [needle for needle in required_logs if needle not in serial]
        if missing_logs:
            raise m4sim.M4SimError(f"reader lifecycle evidence missing: {missing_logs}")

        result = {
            "ok": True,
            "fixture": fixture.name,
            "fixture_bytes": fixture.stat().st_size,
            "boot_ping": boot_ping,
            "protocol": final_ping.get("protocol") if isinstance(final_ping, dict) else None,
            "paths": {
                "reader": _activity_path(reader_ui),
                "quick": _activity_path(quick_ui),
                "progress": _activity_path(progress_ui),
                "final": _activity_path(final_ui),
            },
            "screenshots": [
                p.name for p in (
                    home, library, reader, quick, progress, progress_seek, style,
                    style_adjusted, quick_after_style, more, reader_after
                )
            ],
        }
        (root / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print("READER UI PASS: Home -> MyLibrary -> TxtReader -> Quick/Progress/Style/More -> TxtReader", flush=True)
        print(f"artifacts: {root}", flush=True)
        return result
    except BridgeError as exc:
        raise m4sim.M4SimError(f"m4adb {exc.key}: {exc.message}") from exc
    finally:
        _save_serial(client, root / "artifacts" / "firmware-serial.log")
        if client is not None:
            client.close()
        _terminate(proc)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="m4sim test reader-ui",
        description="Boot a fresh real local-TXT reader touch-UI journey.",
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
    base = Path(os.environ.get("M4SIM_READER_TMP", "/tmp/m4sim-reader-ui"))
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
        result = _run(base, qemu, flash, args.ready_seconds)
        (base / "reader-ui-summary.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        return 0
    except m4sim.M4SimError as exc:
        print(f"READER UI FAIL: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
