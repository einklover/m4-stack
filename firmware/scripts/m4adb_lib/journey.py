"""Declarative journey runner for m4adb."""

from __future__ import annotations

import json
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from . import package as pkg
from .analyzer import analyze_run, write_reports
from .client import BridgeError, Client
from .redact import redact_obj, redact_text


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _unique_run_dir(artifact_root: Path, name: str) -> Path:
    """Collision-free run directory (microseconds + short uuid)."""
    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    suffix = uuid.uuid4().hex[:6]
    return artifact_root / f"{ts}_{name}_{suffix}"


def load_journey(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def run_journey(
    client: Client,
    journey: dict,
    artifact_root: Path,
    cache_dir: Path,
    repo_root: Optional[Path] = None,
) -> tuple[int, Path, dict]:
    """
    Execute journey. Returns (exit_code, run_dir, report).
    All artifact text is redacted before write.
    """
    name = journey.get("name", "journey")
    run_dir = _unique_run_dir(artifact_root, name)
    run_dir.mkdir(parents=True, exist_ok=True)
    shot_dir = run_dir / "screenshots"
    shot_dir.mkdir(exist_ok=True)
    log_path = run_dir / "serial.log"

    timeline: list[dict[str, Any]] = []
    assertions_out: list[dict[str, Any]] = []
    screenshots: dict[str, Path] = {}
    heap_samples: list[int] = []
    psram_samples: list[int] = []
    serial_lines: list[str] = []

    def log_sink(line: str) -> None:
        safe = redact_text(line)
        serial_lines.append(safe)
        with log_path.open("a", encoding="utf-8") as f:
            f.write(f"{_now_iso()} {safe}\n")

    client.log_sink = log_sink

    def sample_status(step: str) -> dict:
        try:
            st = client.status()
            if "free_heap" in st:
                heap_samples.append(int(st["free_heap"]))
            if "free_psram" in st:
                psram_samples.append(int(st["free_psram"]))
            return st
        except Exception as e:
            return {"error": redact_text(str(e))}

    fatal = False
    for idx, step in enumerate(journey.get("steps", [])):
        if fatal:
            break
        action = step.get("action") or step.get("op")
        entry: dict[str, Any] = {
            "step": idx,
            "action": action,
            "ts": _now_iso(),
            "status": "ok",
        }
        try:
            if action in ("install", "sync"):
                src = step.get("path") or step.get("source")
                if not src:
                    raise BridgeError("bad_step", "install/sync 缺少 path")
                p = Path(src)
                if not p.is_absolute() and repo_root:
                    p = (repo_root / p).resolve()
                m4x, ch, mf = pkg.resolve_package(p, cache_dir)
                entry["content_hash"] = ch
                if action == "sync":
                    state_path = cache_dir / "last_sync.json"
                    state = {}
                    if state_path.is_file():
                        state = json.loads(state_path.read_text(encoding="utf-8"))
                    key = str(p)
                    # Host cache cannot prove what is installed on the attached
                    # device. Always ask the device; its content hash check is
                    # authoritative and returns a safe noop when unchanged.
                    res = client.install(m4x)
                    state[key] = ch
                    state_path.write_text(json.dumps(state, indent=2), encoding="utf-8")
                    entry["result"] = res
                else:
                    entry["result"] = client.install(m4x)
            elif action == "launch":
                entry["result"] = client.launch(step["app_id"])
            elif action == "tap":
                entry["result"] = client.tap(step["x"], step["y"])
            elif action == "key":
                entry["result"] = client.key(step["name"])
            elif action == "back":
                entry["result"] = client.back()
            elif action == "home":
                entry["result"] = client.home()
            elif action == "wait":
                time.sleep(float(step.get("ms", 500)) / 1000.0)
                entry["result"] = {"waited_ms": step.get("ms", 500)}
            elif action == "status":
                st = sample_status(f"step{idx}")
                entry["result"] = st
                entry["activity"] = st.get("activity")
                entry["active_app"] = st.get("active_app")
            elif action == "screenshot":
                sname = step.get("name", f"shot_{idx}")
                out = shot_dir / f"{sname}.pbm"
                entry["result"] = client.screenshot(out)
                screenshots[sname] = out
            elif action == "assert":
                st = sample_status(f"assert{idx}")
                entry["activity"] = st.get("activity")
                entry["active_app"] = st.get("active_app")
                ares = _eval_assert(step, st, screenshots, serial_lines, timeline)
                assertions_out.append(ares)
                entry["result"] = ares
                if not ares.get("ok"):
                    entry["status"] = "fail"
            else:
                raise BridgeError("bad_step", f"未知步骤: {action}")
        except BridgeError as e:
            entry["status"] = "fail"
            entry["error_key"] = e.key
            entry["error"] = redact_text(e.message)
            if e.key in ("timeout", "disconnect") or step.get("fatal", False):
                fatal = True
        except Exception as e:
            entry["status"] = "fail"
            entry["error_key"] = "exception"
            entry["error"] = redact_text(str(e))
            fatal = True
        timeline.append(redact_obj(entry))

    report = analyze_run(timeline, serial_lines, screenshots, assertions_out, heap_samples, psram_samples)
    report = redact_obj(report)
    write_reports(run_dir, report, timeline)
    rc = 0 if report.get("ok") else 1
    return rc, run_dir, report


def _eval_assert(
    step: dict,
    status: dict,
    screenshots: dict[str, Path],
    serial_lines: list[str],
    timeline: list[dict],
) -> dict:
    t = step.get("type") or step.get("assert")
    out: dict[str, Any] = {"type": t, "ok": True}
    if t == "activity_equals":
        out["ok"] = status.get("activity") == step.get("value")
        out["actual"] = status.get("activity")
    elif t == "activity_contains":
        out["ok"] = str(step.get("value", "")) in str(status.get("activity", ""))
        out["actual"] = status.get("activity")
    elif t == "active_app_equals":
        out["ok"] = status.get("active_app") == step.get("value")
        out["actual"] = status.get("active_app")
    elif t == "log_contains":
        needle = step.get("value", "")
        out["ok"] = any(needle in ln for ln in serial_lines)
    elif t == "log_not_contains":
        needle = step.get("value", "")
        out["ok"] = not any(needle in ln for ln in serial_lines)
    elif t == "min_heap":
        thr = int(step.get("value", 0))
        heap = int(status.get("free_heap") or 0)
        out["ok"] = heap >= thr
        out["actual"] = heap
    elif t == "screenshot_changed":
        a = step.get("from")
        b = step.get("name") or step.get("to")
        if a not in screenshots or b not in screenshots:
            out["ok"] = False
            out["error"] = "missing screenshot"
        else:
            out["ok"] = screenshots[a].read_bytes() != screenshots[b].read_bytes()
    elif t == "command_success":
        # Refer to a real prior timeline step: explicit "step" index, else previous step.
        if "step" in step:
            ref = int(step["step"])
        else:
            ref = len(timeline) - 1
        out["ref_step"] = ref
        if ref < 0 or ref >= len(timeline):
            out["ok"] = False
            out["error"] = "invalid_step_ref"
        else:
            out["ok"] = timeline[ref].get("status") == "ok"
            out["ref_status"] = timeline[ref].get("status")
            out["ref_action"] = timeline[ref].get("action")
    else:
        out["ok"] = False
        out["error"] = f"unknown assert {t}"
    return out
