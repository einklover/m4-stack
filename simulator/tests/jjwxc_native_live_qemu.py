#!/usr/bin/env python3
"""Issue #19: drive the real native JJWXC path in existing Murphy QEMU.

The harness only fixes the discovery entry row (book 3006768). Catalog HTTPS,
streaming parse, SD commit, TOC, chapter fetch and native TxtReader remain the
production firmware path. Raw catalog/chapter content is never kept as an
artifact; only state/memory/checkpoint metadata is retained.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
M4SIM = ROOT / "simulator/m4sim.py"
M4ADB = ROOT / "firmware/scripts/m4adb.py"
BOOK_ID = "3006768"
APP_ID = "com.jjwxc.client"


def run(cmd: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None,
        timeout: float | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd), flush=True)
    cp = subprocess.run(cmd, cwd=cwd, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=timeout, check=False)
    if cp.stdout:
        print(cp.stdout, end="" if cp.stdout.endswith("\n") else "\n", flush=True)
    if check and cp.returncode != 0:
        raise RuntimeError(f"command failed rc={cp.returncode}: {' '.join(cmd)}")
    return cp


def first_json(text: str) -> dict[str, Any] | None:
    dec = json.JSONDecoder()
    for i, c in enumerate(text):
        if c != "{":
            continue
        try:
            obj, _ = dec.raw_decode(text[i:])
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            return obj
    return None


def m4adb(pty: str, args: list[str], *, timeout: float = 30.0, check: bool = True) -> tuple[subprocess.CompletedProcess[str], dict[str, Any] | None]:
    cp = run([
        sys.executable, str(M4ADB), "--port", pty, "--no-daemon",
        "--timeout", "20", "--ready-timeout", "20", *args,
    ], timeout=timeout, check=check)
    return cp, first_json(cp.stdout or "")


def nested_body(ui: dict[str, Any] | None) -> dict[str, Any]:
    if not isinstance(ui, dict):
        return {}
    outer = ui.get("ui")
    if not isinstance(outer, dict):
        return {}
    body = outer.get("body")
    return body if isinstance(body, dict) else {}


def wait_ui(pty: str, predicate, out: Path, *, seconds: int) -> dict[str, Any]:
    deadline = time.time() + seconds
    last: dict[str, Any] | None = None
    last_text = ""
    while time.time() < deadline:
        cp, obj = m4adb(pty, ["ui"], timeout=25, check=False)
        last_text = cp.stdout or ""
        if obj:
            last = obj
            if predicate(obj):
                out.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
                return obj
        time.sleep(1)
    out.write_text(json.dumps(last, ensure_ascii=False, indent=2) + "\n" if last else last_text,
                   encoding="utf-8")
    raise RuntimeError(f"UI condition timeout after {seconds}s; last={last}")


def status(pty: str, path: Path) -> dict[str, Any]:
    _, obj = m4adb(pty, ["status"], timeout=25)
    if not obj:
        raise RuntimeError("status JSON missing")
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return obj


def screenshot(pty: str, path: Path) -> None:
    m4adb(pty, ["screenshot", str(path)], timeout=45, check=False)


def seed_sd(sd: Path, temp: Path) -> None:
    seed = temp / "shelf_rows.tsv"
    seed.write_text(f"{BOOK_ID}\tIssue19 Live Book\tJJWXC\t0\n", encoding="utf-8")
    for d in ("/apps_data", f"/apps_data/{APP_ID}", f"/apps_data/{APP_ID}/provider"):
        run(["mmd", "-i", str(sd), f"::{d}"], check=False)
    run(["mcopy", "-o", "-i", str(sd), str(seed), f"::/apps_data/{APP_ID}/provider/shelf_rows.tsv"])
    seed.unlink(missing_ok=True)


def inspect_sd(sd: Path, temp: Path) -> dict[str, int]:
    extracted = temp / "toc_rows.txt"
    cp = run(["mcopy", "-i", str(sd),
              f"::/apps_data/{APP_ID}/cache/{BOOK_ID}/toc_rows.txt", str(extracted)], check=False)
    if cp.returncode != 0 or not extracted.is_file():
        return {"catalog_sd_bytes": 0, "catalog_sd_rows": 0}
    nbytes = extracted.stat().st_size
    with extracted.open("rb") as f:
        rows = sum(chunk.count(b"\n") for chunk in iter(lambda: f.read(65536), b""))
    extracted.unlink(missing_ok=True)
    return {"catalog_sd_bytes": nbytes, "catalog_sd_rows": rows}


def key(pty: str, name: str) -> None:
    m4adb(pty, ["key", name], timeout=25)


def qemu_checkpoint_lines(qlog: Path) -> list[str]:
    if not qlog.is_file():
        return []
    keep = re.compile(r"\[NativeCatalog\]|\[NativeProvider|Guru Meditation|Brownout|abort\(\)|heap", re.I)
    lines = []
    for line in qlog.read_text(encoding="utf-8", errors="replace").splitlines():
        if keep.search(line):
            lines.append(line[:500])
    return lines[-300:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu-source", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--artifacts", type=Path, required=True)
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    work = args.work.resolve()
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["M4SIM_TMP"] = str(work / "m4sim")
    m4tmp = Path(env["M4SIM_TMP"])
    m4art = m4tmp / "artifacts"
    m4art.mkdir(parents=True, exist_ok=True)

    pty = ""
    qlog = Path()
    sd = m4art / "murphy-sd.img"
    states: dict[str, dict[str, Any]] = {}
    try:
        if not args.skip_build:
            run(["bash", "scripts/bootstrap_deps.sh"])
            run(["pio", "run", "-e", "murphy_m4_qemu_plugin"], cwd=ROOT / "firmware", timeout=1800)
            run([sys.executable, "simulator/qemu/build.py", "--source-dir", str(args.qemu_source), "--reconfigure", "-j", "2"], timeout=2400)

        qemu = args.qemu_source / "build-murphy-v3/qemu-system-xtensa"
        if not qemu.is_file() and Path(str(qemu) + "-unsigned").is_file():
            qemu = Path(str(qemu) + "-unsigned")
        if not qemu.is_file():
            raise RuntimeError(f"QEMU missing: {qemu}")

        run([sys.executable, "simulator/qemu/make_sd_image.py", str(sd), "--size-mb", "64"])
        seed_sd(sd, work)

        run([sys.executable, str(M4SIM), "run", "--plugin-debug", "--skip-build",
             "--qemu", str(qemu), "--ready-seconds", "150", "--force"], env=env, timeout=240)
        st = json.loads((m4tmp / "state.json").read_text(encoding="utf-8"))
        pty = st["pty"]
        qlog = Path(st["qemu_log"])
        sd = Path(st["sd"])

        states["before"] = status(pty, artifacts / "status-before.json")
        m4adb(pty, ["install", "plugins/m4-jjwxc-plugin", "--transport", "usb",
                     "--ready-timeout", "20", "--commit-timeout", "120", "--overall-timeout", "180"], timeout=210)
        m4adb(pty, ["launch", APP_ID], timeout=40)

        wait_ui(pty, lambda x: nested_body(x).get("kind") == "native_app" and nested_body(x).get("rows") == 1,
                artifacts / "ui-home.json", seconds=60)
        screenshot(pty, artifacts / "home.pbm")

        key(pty, "confirm")
        wait_ui(pty, lambda x: nested_body(x).get("kind") == "native_provider_book" and
                                  nested_body(x).get("book") == BOOK_ID and nested_body(x).get("state") == 0,
                artifacts / "ui-detail.json", seconds=90)
        states["detail"] = status(pty, artifacts / "status-detail.json")

        key(pty, "Left")
        wait_ui(pty, lambda x: nested_body(x).get("kind") == "native_provider_book" and nested_body(x).get("state") == 2,
                artifacts / "ui-toc.json", seconds=150)
        states["toc"] = status(pty, artifacts / "status-toc.json")
        screenshot(pty, artifacts / "toc.pbm")

        key(pty, "confirm")
        wait_ui(pty, lambda x: nested_body(x).get("kind") == "native_provider_book" and nested_body(x).get("state") == 5,
                artifacts / "ui-reader.json", seconds=180)
        states["reader"] = status(pty, artifacts / "status-reader.json")
        screenshot(pty, artifacts / "reader.pbm")

        # Allow the same catalog worker to finish its background full refill.
        deadline = time.time() + 90
        while time.time() < deadline:
            if qlog.is_file() and "[NativeCatalog] progressive full ready" in qlog.read_text(encoding="utf-8", errors="replace"):
                break
            time.sleep(1)

    except Exception as exc:
        (artifacts / "failure.txt").write_text(str(exc) + "\n", encoding="utf-8")
        print(f"FAIL: {exc}", file=sys.stderr)
        rc = 1
    else:
        rc = 0
    finally:
        run([sys.executable, str(M4SIM), "stop"], env=env, check=False, timeout=20)
        sd_meta = inspect_sd(sd, work) if sd.is_file() else {"catalog_sd_bytes": 0, "catalog_sd_rows": 0}
        checkpoints = qemu_checkpoint_lines(qlog)
        (artifacts / "runtime-checkpoints.log").write_text("\n".join(checkpoints) + "\n", encoding="utf-8")
        compact: dict[str, Any] = {}
        for name, s in states.items():
            compact[name] = {k: s.get(k) for k in
                             ("free_heap", "min_free_heap", "free_psram", "reset_reason", "activity", "active_app")}
        evidence = {
            "book_id": BOOK_ID,
            "memory": compact,
            **sd_meta,
            "catalog_first_window_seen": any("progressive first-window ready" in x for x in checkpoints),
            "catalog_full_ready_seen": any("progressive full ready" in x for x in checkpoints),
            "fatal_reset_seen": any(re.search(r"Guru Meditation|Brownout|abort\(\)", x, re.I) for x in checkpoints),
            "raw_content_retained": False,
        }
        (artifacts / "qemu-evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(evidence, ensure_ascii=False, indent=2))
        if rc == 0:
            if sd_meta["catalog_sd_rows"] < 24 or sd_meta["catalog_sd_bytes"] <= 0:
                print("FAIL: committed catalog metadata missing", file=sys.stderr)
                rc = 1
            if evidence["fatal_reset_seen"]:
                print("FAIL: fatal reset signature seen", file=sys.stderr)
                rc = 1
            resets = {v.get("reset_reason") for v in compact.values() if v.get("reset_reason") is not None}
            if len(resets) > 1:
                print("FAIL: reset reason changed during run", file=sys.stderr)
                rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
