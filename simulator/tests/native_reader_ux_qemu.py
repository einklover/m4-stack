#!/usr/bin/env python3
"""Issue #25: real Fanqie / WeRead native-reader journeys in Murphy QEMU.

This driver does not mock provider HTTP or the reader. It drives the same
m4adb key/touch surface a user would use and verifies structured UI states.
WeRead is split into start/finish so CI can publish the *same-session* QR as a
short-lived artifact while QEMU keeps polling for the user's scan.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[2]
M4SIM = ROOT / "simulator/m4sim.py"
M4ADB = ROOT / "firmware/scripts/m4adb.py"
FANQIE_APP = "com.fanqie.client"
WEREAD_APP = "com.weread.client"


def run(cmd: list[str], *, env: dict[str, str] | None = None, timeout: float | None = None,
        check: bool = True, echo_output: bool = True) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd), flush=True)
    cp = subprocess.run(cmd, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    if echo_output and cp.stdout:
        print(cp.stdout, end="" if cp.stdout.endswith("\n") else "\n", flush=True)
    if check and cp.returncode != 0:
        raise RuntimeError(f"command failed rc={cp.returncode}: {' '.join(cmd)}")
    return cp


def first_json(text: str) -> dict[str, Any] | None:
    dec = json.JSONDecoder()
    for i, ch in enumerate(text):
        if ch != "{":
            continue
        try:
            obj, _ = dec.raw_decode(text[i:])
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            return obj
    return None


def nested_body(obj: dict[str, Any] | None) -> dict[str, Any]:
    if not isinstance(obj, dict):
        return {}
    ui = obj.get("ui")
    if not isinstance(ui, dict):
        return {}
    body = ui.get("body")
    return body if isinstance(body, dict) else {}


def m4adb(pty: str, args: list[str], *, timeout: float = 35.0, check: bool = True,
          quiet: bool = False) -> tuple[subprocess.CompletedProcess[str], dict[str, Any] | None]:
    cp = run([
        sys.executable, str(M4ADB), "--port", pty, "--no-daemon",
        "--timeout", "20", "--ready-timeout", "20", *args,
    ], timeout=timeout, check=check, echo_output=not quiet)
    return cp, first_json(cp.stdout or "")


def wait_ui(pty: str, pred: Callable[[dict[str, Any]], bool], path: Path, *, seconds: int,
            quiet: bool = False) -> dict[str, Any]:
    deadline = time.time() + seconds
    last: dict[str, Any] | None = None
    while time.time() < deadline:
        _, obj = m4adb(pty, ["ui"], timeout=25, check=False, quiet=quiet)
        if obj:
            last = obj
            if pred(obj):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
                return obj
        time.sleep(1)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(last, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    raise RuntimeError(f"UI condition timeout after {seconds}s; last body={nested_body(last)}")


def save_status(pty: str, path: Path) -> dict[str, Any]:
    _, obj = m4adb(pty, ["status"], timeout=25)
    if not obj:
        raise RuntimeError("status JSON missing")
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return obj


def screenshot(pty: str, path: Path) -> None:
    m4adb(pty, ["screenshot", str(path)], timeout=50, check=False, quiet=True)


def key(pty: str, name: str) -> None:
    m4adb(pty, ["key", name], timeout=25, quiet=True)
    time.sleep(0.35)


def sim_env(work: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["M4SIM_TMP"] = str(work / "m4sim")
    return env


def resolve_qemu(qemu_source: Path) -> Path:
    qemu = qemu_source / "build-murphy-v3/qemu-system-xtensa"
    if not qemu.is_file() and Path(str(qemu) + "-unsigned").is_file():
        qemu = Path(str(qemu) + "-unsigned")
    if not qemu.is_file():
        raise RuntimeError(f"QEMU missing: {qemu}")
    return qemu


def start_sim(qemu_source: Path, work: Path) -> tuple[str, dict[str, Any]]:
    env = sim_env(work)
    qemu = resolve_qemu(qemu_source)
    run([sys.executable, str(M4SIM), "run", "--plugin-debug", "--skip-build",
         "--qemu", str(qemu), "--ready-seconds", "150", "--force"],
        env=env, timeout=240)
    state_path = Path(env["M4SIM_TMP"]) / "state.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    pty = str(state.get("pty") or "")
    if not pty:
        raise RuntimeError("m4sim state missing PTY")
    return pty, state


def stop_sim(work: Path) -> None:
    run([sys.executable, str(M4SIM), "stop"], env=sim_env(work), timeout=30, check=False)


def install_launch(pty: str, plugin: str, app_id: str) -> None:
    m4adb(pty, ["install", plugin, "--transport", "usb", "--ready-timeout", "20",
                 "--commit-timeout", "120", "--overall-timeout", "180"], timeout=220)
    m4adb(pty, ["launch", app_id], timeout=45)


def body_kind(kind: str) -> Callable[[dict[str, Any]], bool]:
    return lambda x: nested_body(x).get("kind") == kind


def wait_book_state(pty: str, provider: str, state: int, path: Path, seconds: int) -> dict[str, Any]:
    return wait_ui(
        pty,
        lambda x: nested_body(x).get("kind") == "native_provider_book"
        and nested_body(x).get("provider") == provider
        and nested_body(x).get("state") == state,
        path, seconds=seconds,
    )


def fanqie(qemu_source: Path, work: Path, artifacts: Path) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    pty = ""
    evidence: dict[str, Any] = {"provider": "fanqie", "steps": []}
    try:
        pty, _ = start_sim(qemu_source, work)
        save_status(pty, artifacts / "00-status-boot.json")
        install_launch(pty, "plugins/m4-fanqie-plugin", FANQIE_APP)

        home = wait_ui(
            pty,
            lambda x: nested_body(x).get("kind") == "native_app"
            and nested_body(x).get("provider") == "fanqie"
            and int(nested_body(x).get("tiles") or 0) == 8
            and int(nested_body(x).get("rows") or 0) > 0,
            artifacts / "01-home.json", seconds=150,
        )
        hb = nested_body(home)
        if hb.get("focus") != "tiles" or int(hb.get("tile_selected") or -1) != 0:
            raise RuntimeError(f"Fanqie initial tile focus invalid: {hb}")
        screenshot(pty, artifacts / "01-home.pbm")
        evidence["steps"].append("home_live_rows")

        key(pty, "Right")
        moved = wait_ui(
            pty,
            lambda x: nested_body(x).get("focus") == "tiles"
            and int(nested_body(x).get("tile_selected") or -1) == 1,
            artifacts / "02-tile-right.json", seconds=15,
        )
        if int(nested_body(moved).get("tiles") or 0) != 8:
            raise RuntimeError("Fanqie tile count changed unexpectedly")
        key(pty, "confirm")
        time.sleep(3)
        screenshot(pty, artifacts / "02-category-selected.pbm")
        evidence["steps"].append("tile_key_select")

        key(pty, "Down")  # 1 -> 5, second tile row
        wait_ui(pty, lambda x: nested_body(x).get("focus") == "tiles"
                and int(nested_body(x).get("tile_selected") or -1) == 5,
                artifacts / "03-tile-down.json", seconds=15)
        key(pty, "Down")  # last tile row -> list
        list_ui = wait_ui(pty, lambda x: nested_body(x).get("focus") == "list"
                          and int(nested_body(x).get("rows") or 0) > 0,
                          artifacts / "04-list-focus.json", seconds=30)
        screenshot(pty, artifacts / "04-list-focus.pbm")
        evidence["steps"].append("tiles_to_list_focus")

        key(pty, "confirm")
        detail = wait_book_state(pty, "fanqie", 0, artifacts / "05-detail.json", 120)
        book_id = str(nested_body(detail).get("book") or "")
        if not book_id:
            raise RuntimeError("Fanqie detail has no book id")
        evidence["book_id"] = book_id
        screenshot(pty, artifacts / "05-detail.pbm")
        save_status(pty, artifacts / "05-status-detail.json")
        evidence["steps"].append("real_book_detail")

        key(pty, "Left")
        wait_book_state(pty, "fanqie", 2, artifacts / "06-toc.json", 180)
        screenshot(pty, artifacts / "06-toc.pbm")
        evidence["steps"].append("toc")

        key(pty, "confirm")
        wait_book_state(pty, "fanqie", 5, artifacts / "07-reader.json", 240)
        screenshot(pty, artifacts / "07-reader-page1.pbm")
        save_status(pty, artifacts / "07-status-reader.json")
        evidence["steps"].append("reader_open")

        # Real reader page turn through the same mapped Right key a user uses.
        key(pty, "Right")
        time.sleep(2)
        screenshot(pty, artifacts / "08-reader-page2.pbm")
        evidence["steps"].append("reader_page_turn")

        key(pty, "back")
        wait_book_state(pty, "fanqie", 0, artifacts / "09-detail-after-reader.json", 60)
        key(pty, "confirm")
        wait_book_state(pty, "fanqie", 5, artifacts / "10-reader-resume.json", 180)
        screenshot(pty, artifacts / "10-reader-resume.pbm")
        evidence["steps"].append("reader_resume")

        evidence["ok"] = True
        (artifacts / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
                                                 encoding="utf-8")
        return 0
    except Exception as exc:
        evidence["ok"] = False
        evidence["error"] = str(exc)
        (artifacts / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
                                                 encoding="utf-8")
        print(f"FANQIE FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        stop_sim(work)


def weread_start(qemu_source: Path, work: Path, artifacts: Path) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    try:
        pty, _ = start_sim(qemu_source, work)
        save_status(pty, artifacts / "00-status-boot.json")
        install_launch(pty, "plugins/m4-weread-plugin", WEREAD_APP)
        qr_ui = wait_ui(
            pty,
            lambda x: nested_body(x).get("kind") == "native_provider_login"
            and bool(nested_body(x).get("has_qr"))
            and bool(nested_body(x).get("qr_url")),
            artifacts / "01-qr-ui-private.json", seconds=150, quiet=True,
        )
        body = nested_body(qr_ui)
        qr = str(body.get("qr_url") or "")
        if not qr.startswith("https://weread.qq.com/web/confirm?uid="):
            raise RuntimeError("unexpected WeRead QR URL")
        # Keep the sensitive URL out of stdout. This file is uploaded as a
        # short-lived artifact; the running QEMU process remains the poll owner.
        (artifacts / "qr.json").write_text(json.dumps({"qr_url": qr}, ensure_ascii=False) + "\n",
                                           encoding="utf-8")
        screenshot(pty, artifacts / "01-qr-screen.pbm")
        (artifacts / "started.json").write_text(json.dumps({"ok": True, "pty": pty}) + "\n",
                                                encoding="utf-8")
        print("WEREAD_QR_READY same-session artifact written", flush=True)
        return 0
    except Exception as exc:
        (artifacts / "started.json").write_text(json.dumps({"ok": False, "error": str(exc)}) + "\n",
                                                encoding="utf-8")
        print(f"WEREAD START FAIL: {exc}", file=sys.stderr)
        stop_sim(work)
        return 1


def weread_finish(work: Path, artifacts: Path) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    evidence: dict[str, Any] = {"provider": "weread", "steps": ["same_session_qr"]}
    try:
        state = json.loads((work / "m4sim/state.json").read_text(encoding="utf-8"))
        pty = str(state.get("pty") or "")
        if not pty:
            raise RuntimeError("persisted WeRead QEMU PTY missing")

        home = wait_ui(
            pty,
            lambda x: nested_body(x).get("kind") == "native_app"
            and nested_body(x).get("provider") == "weread"
            and int(nested_body(x).get("rows") or 0) > 0,
            artifacts / "02-home-after-login.json", seconds=540,
        )
        hb = nested_body(home)
        if int(hb.get("tiles") or 0) != 0:
            raise RuntimeError(f"WeRead must not expose discovery tiles: {hb}")
        screenshot(pty, artifacts / "02-home-after-login.pbm")
        save_status(pty, artifacts / "02-status-home.json")
        evidence["steps"].append("login_and_bookshelf_sync")

        key(pty, "confirm")
        detail = wait_book_state(pty, "weread", 0, artifacts / "03-detail.json", 150)
        book_id = str(nested_body(detail).get("book") or "")
        if not book_id:
            raise RuntimeError("WeRead detail has no book id")
        evidence["book_id"] = book_id
        screenshot(pty, artifacts / "03-detail.pbm")
        evidence["steps"].append("real_book_detail")

        key(pty, "Left")
        wait_book_state(pty, "weread", 2, artifacts / "04-toc.json", 240)
        screenshot(pty, artifacts / "04-toc.pbm")
        evidence["steps"].append("toc")

        key(pty, "confirm")
        wait_book_state(pty, "weread", 5, artifacts / "05-reader.json", 300)
        screenshot(pty, artifacts / "05-reader-page1.pbm")
        save_status(pty, artifacts / "05-status-reader.json")
        evidence["steps"].append("reader_open")

        key(pty, "Right")
        time.sleep(2)
        screenshot(pty, artifacts / "06-reader-page2.pbm")
        evidence["steps"].append("reader_page_turn")

        key(pty, "back")
        wait_book_state(pty, "weread", 0, artifacts / "07-detail-after-reader.json", 90)
        key(pty, "confirm")
        wait_book_state(pty, "weread", 5, artifacts / "08-reader-resume.json", 240)
        screenshot(pty, artifacts / "08-reader-resume.pbm")
        evidence["steps"].append("reader_resume")

        evidence["ok"] = True
        (artifacts / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
                                                 encoding="utf-8")
        return 0
    except Exception as exc:
        evidence["ok"] = False
        evidence["error"] = str(exc)
        (artifacts / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
                                                 encoding="utf-8")
        print(f"WEREAD FINISH FAIL: {exc}", file=sys.stderr)
        return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("fanqie", "weread-start", "weread-finish", "stop"))
    ap.add_argument("--qemu-source", type=Path)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--artifacts", type=Path, required=True)
    args = ap.parse_args()
    work = args.work.resolve()
    artifacts = args.artifacts.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.mode == "stop":
        stop_sim(work)
        return 0
    if args.mode in ("fanqie", "weread-start") and not args.qemu_source:
        ap.error("--qemu-source is required for start modes")
    if args.mode == "fanqie":
        return fanqie(args.qemu_source.resolve(), work, artifacts)
    if args.mode == "weread-start":
        return weread_start(args.qemu_source.resolve(), work, artifacts)
    return weread_finish(work, artifacts)


if __name__ == "__main__":
    raise SystemExit(main())
