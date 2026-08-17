#!/usr/bin/env python3
"""Real local-TXT reader overlay journey through production Home/MyLibrary/QEMU."""
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
FIXTURE_NAME = "reader-ui-fixture.txt"
READER_PATH = ("Reader", "TxtReader")
MENU_PATH = ("Reader", "TxtReader", "EpubReaderMenu")
CHAPTER_PATH = ("Reader", "TxtReader", "TxtReaderChapterSelection")
PROGRESS_PATH = ("Reader", "TxtReader", "EpubReaderPercentSelection")
BOOKMARK_PATH = ("Reader", "TxtReader", "BookmarkManager")


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


def _top_body(ui: dict[str, Any]) -> dict[str, Any]:
    outer = ui.get("ui")
    if not isinstance(outer, dict):
        return {}
    body = outer.get("body")
    return body if isinstance(body, dict) else {}


def _deepest_body(ui: dict[str, Any]) -> dict[str, Any]:
    body: Any = _top_body(ui)
    for _ in range(8):
        if not isinstance(body, dict):
            return {}
        child = body.get("child")
        if not isinstance(child, dict):
            return body
        body = child
    return body if isinstance(body, dict) else {}


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


def _wait_library_state(client: Client, proc: Any, qlog: Path, *, seconds: float = 12.0) -> dict[str, Any]:
    deadline = time.monotonic() + seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise m4sim.M4SimError(f"QEMU exited in MyLibrary\n{_qemu_tail(qlog)}")
        try:
            last = _ui(client)
        except m4sim.M4SimError:
            time.sleep(0.15)
            continue
        if _activity_path(last)[:1] == ("MyLibrary",):
            body = _top_body(last)
            if isinstance(body.get("selected_index"), int) and isinstance(body.get("count"), int):
                return last
        time.sleep(0.15)
    raise m4sim.M4SimError(f"MyLibrary selection state unavailable: {last!r}")


def _select_library_entry(client: Client, proc: Any, qlog: Path, target: str) -> dict[str, Any]:
    state = _wait_library_state(client, proc, qlog)
    for _ in range(64):
        body = _top_body(state)
        if body.get("selected") == target:
            return state
        count = body.get("count")
        index = body.get("selected_index")
        if not isinstance(count, int) or count <= 0 or not isinstance(index, int):
            raise m4sim.M4SimError(f"invalid MyLibrary state: {body!r}")
        before = (index, str(body.get("selected", "")))
        _send_key(client, "down")
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            state = _wait_library_state(client, proc, qlog, seconds=max(0.5, deadline - time.monotonic()))
            after_body = _top_body(state)
            after = (after_body.get("selected_index"), str(after_body.get("selected", "")))
            if after != before:
                break
            time.sleep(0.1)
        else:
            raise m4sim.M4SimError(f"MyLibrary selection did not move from {before!r}")
    raise m4sim.M4SimError(f"MyLibrary did not contain {target!r}; last={_top_body(state)!r}")


def _capture(client: Client, root: Path, name: str) -> Path:
    out = root / "artifacts" / f"{name}.pbm"
    out.parent.mkdir(parents=True, exist_ok=True)
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
    fixture = root / FIXTURE_NAME
    lines = [
        "墨水屏阅读器 QEMU 触控界面回归测试",
        "这是一份仅用于模拟器的 UTF-8 本地 TXT。",
        "目录、进度、字体和更多必须保持阅读上下文。",
        "",
    ]
    for i in range(1, 241):
        lines.append(
            f"第{i:03d}段  天地玄黄，宇宙洪荒。阅读体验应保持清晰、克制、稳定；"
            "触控操作采用离散提交，避免墨水屏连续重绘。"
        )
    fixture.write_text("\n".join(lines) + "\n", encoding="utf-8")
    cp = subprocess.run(
        [mcopy, "-o", "-i", str(sd), str(fixture), f"::/{FIXTURE_NAME}"],
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

        _wait_top(client, proc, qlog, "Home", seconds=30.0)
        home = _capture(client, root, "01-home")
        _send_key(client, "confirm")
        _wait_top(client, proc, qlog, "MyLibrary", seconds=20.0)
        _select_library_entry(client, proc, qlog, FIXTURE_NAME)
        library = _capture(client, root, "02-library-fixture-selected")
        _send_key(client, "confirm")
        reader_ui = _wait_path(client, proc, qlog, READER_PATH, seconds=60.0)
        reader = _capture(client, root, "03-reader")

        # Reader -> overlay bars. A tap in exposed text must dismiss them.
        _send_key(client, "confirm")
        quick_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        quick_body = _deepest_body(quick_ui)
        if quick_body.get("layer") != "quick" or quick_body.get("overlay") is not True or quick_body.get("items") != 4:
            raise m4sim.M4SimError(f"quick overlay contract violated: {quick_body!r}")
        quick = _capture(client, root, "04-overlay-bars")
        _tap(client, 240, 320)
        _wait_path(client, proc, qlog, READER_PATH, seconds=30.0)
        reader_after_dismiss = _capture(client, root, "05-reader-after-overlay-dismiss")
        _assert_changed(quick, reader_after_dismiss, "tap reading text dismisses overlay")

        # Catalog remains a dedicated page. Use the visible top-left Back hitbox.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 60, 756)
        chapter_ui = _wait_path(client, proc, qlog, CHAPTER_PATH, seconds=30.0)
        time.sleep(1.5)
        chapter = _capture(client, root, "06-txt-catalog")
        _tap(client, 24, 28)
        _wait_path(client, proc, qlog, READER_PATH, seconds=30.0)

        # Progress is a bottom sheet; changing the target does not leave reading.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 180, 756)
        progress_ui = _wait_path(client, proc, qlog, PROGRESS_PATH, seconds=20.0)
        progress = _capture(client, root, "07-progress-sheet")
        _tap(client, 347, 625)
        progress_seek = _capture(client, root, "08-progress-sheet-seek")
        _assert_changed(progress, progress_seek, "progress target change")
        _tap(client, 240, 300)
        _wait_path(client, proc, qlog, READER_PATH, seconds=30.0)

        # Font is a bottom sheet. A- updates only the sheet; exposed text closes it
        # and lets the reader apply the deferred reflow.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 300, 756)
        style_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        style_body = _deepest_body(style_ui)
        if style_body.get("layer") != "style" or style_body.get("overlay") is not True:
            raise m4sim.M4SimError(f"font overlay contract violated: {style_body!r}")
        style = _capture(client, root, "09-font-sheet")
        _tap(client, 90, 620)
        style_adjusted = _capture(client, root, "10-font-sheet-minus")
        _assert_changed(style, style_adjusted, "font size decrease")
        _tap(client, 240, 300)
        _wait_path(client, proc, qlog, READER_PATH, seconds=40.0)
        reader_after_style = _capture(client, root, "11-reader-after-font")

        # More remains the existing full secondary list and TXT hides sync rows.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 420, 756)
        more_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        more_body = _deepest_body(more_ui)
        if more_body.get("layer") != "more" or more_body.get("overlay") is not False or more_body.get("has_sync") is not False:
            raise m4sim.M4SimError(f"TXT More capability contract violated: {more_body!r}")
        more = _capture(client, root, "12-more-full-page")
        _send_key(client, "back")
        _wait_path(client, proc, qlog, READER_PATH, seconds=40.0)

        # Bookmark stays accessible without cluttering the bottom toolbar:
        # top-right adds one; More -> bookmark manager opens the existing page.
        _send_key(client, "confirm")
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        _tap(client, 450, 28)
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        bookmark_added = _capture(client, root, "13-bookmark-added-from-topbar")
        _tap(client, 420, 756)
        _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)
        for _ in range(8):
            _send_key(client, "down")
        _send_key(client, "confirm")
        bookmark_ui = _wait_path(client, proc, qlog, BOOKMARK_PATH, seconds=30.0)
        bookmark_list = _capture(client, root, "14-bookmark-manager")
        _send_key(client, "back")
        final_ui = _wait_path(client, proc, qlog, READER_PATH, seconds=30.0)
        reader_final = _capture(client, root, "15-reader-final")

        _assert_changed(home, library, "home -> library")
        _assert_changed(library, reader, "library -> reader")
        _assert_changed(reader, quick, "reader -> overlay")
        _assert_changed(quick, chapter, "overlay -> catalog")
        _assert_changed(more, reader_after_style, "More is distinct from reader")
        _assert_changed(bookmark_added, bookmark_list, "More -> bookmark manager")
        _assert_changed(bookmark_list, reader_final, "bookmark manager -> reader")

        final_ping = _call_busy_retry(client.ping)
        serial = "\n".join(client.serial_log)
        required_logs = (
            "Entering activity: TxtReader",
            "Entering activity: EpubReaderMenu",
            "Entering activity: TxtReaderChapterSelection",
            "Entering activity: EpubReaderPercentSelection",
            "Entering activity: BookmarkManager",
        )
        missing = [needle for needle in required_logs if needle not in serial]
        if missing:
            raise m4sim.M4SimError(f"reader lifecycle evidence missing: {missing}")

        result = {
            "ok": True,
            "fixture": fixture.name,
            "fixture_bytes": fixture.stat().st_size,
            "boot_ping": boot_ping,
            "protocol": final_ping.get("protocol") if isinstance(final_ping, dict) else None,
            "paths": {
                "reader": _activity_path(reader_ui),
                "quick": _activity_path(quick_ui),
                "chapter": _activity_path(chapter_ui),
                "progress": _activity_path(progress_ui),
                "style": _activity_path(style_ui),
                "bookmark": _activity_path(bookmark_ui),
                "final": _activity_path(final_ui),
            },
            "quick": quick_body,
            "style": style_body,
            "txt_more": more_body,
            "screenshots": [
                p.name for p in (
                    home, library, reader, quick, reader_after_dismiss, chapter,
                    progress, progress_seek, style, style_adjusted, reader_after_style,
                    more, bookmark_added, bookmark_list, reader_final
                )
            ],
        }
        (root / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(
            "READER UI PASS: Reader overlays + Catalog/More full pages + Bookmark manager",
            flush=True,
        )
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
        description="Boot a fresh real local-TXT reader overlay journey.",
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
