"""Host-side deterministic analyzer for m4adb journey runs."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any


FATAL_PATTERNS = [
    (re.compile(r"Guru Meditation|abort\(\)|Brownout|rst:0x", re.I), "reboot_reset"),
    (re.compile(r"Lua traceback|PANIC|runtime fatal|M4x.*fatal", re.I), "app_fatal"),
    (re.compile(r"SSL|TLS|mbedtls|certificate|ECONN|timeout.*http|HTTP.*fail", re.I), "network_tls"),
    (re.compile(r"SD.*(fail|error|io_failure)|no sd|sd_ok=false", re.I), "sd_error"),
    (re.compile(r"protocol error|@M4DBG.*err", re.I), "protocol_error"),
]


def analyze_run(
    timeline: list[dict[str, Any]],
    serial_lines: list[str],
    screenshots: dict[str, Path],
    assertions: list[dict[str, Any]],
    heap_samples: list[int],
    psram_samples: list[int],
) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    for line in serial_lines:
        for pat, kind in FATAL_PATTERNS:
            if pat.search(line):
                findings.append({"kind": kind, "line": line[:200]})
                break

    failed_steps = [t for t in timeline if t.get("status") == "fail"]
    for t in timeline:
        if t.get("error_key") in ("timeout", "disconnect"):
            findings.append({"kind": t["error_key"], "step": t.get("step")})

    # Screenshot identity when change expected
    for a in assertions:
        if a.get("type") == "screenshot_changed" and a.get("ok") is False:
            findings.append(
                {
                    "kind": "screenshot_unchanged",
                    "from": a.get("from"),
                    "to": a.get("name"),
                }
            )

    heap_info = {}
    if heap_samples:
        heap_info = {
            "start": heap_samples[0],
            "end": heap_samples[-1],
            "min": min(heap_samples),
            "max": max(heap_samples),
        }
        # Heuristic leak: steadily decreasing across >=4 samples by >10% of start
        if len(heap_samples) >= 4 and heap_samples[0] > 0:
            drop = heap_samples[0] - heap_samples[-1]
            if drop > heap_samples[0] * 0.10 and all(
                heap_samples[i] >= heap_samples[i + 1] for i in range(len(heap_samples) - 1)
            ):
                findings.append(
                    {
                        "kind": "memory_leak_heuristic",
                        "note": "internal heap monotonically decreased >10% (heuristic, not proof)",
                        "start": heap_samples[0],
                        "end": heap_samples[-1],
                    }
                )

    psram_info = {}
    if psram_samples:
        psram_info = {
            "start": psram_samples[0],
            "end": psram_samples[-1],
            "min": min(psram_samples),
        }

    failed_assertions = [a for a in assertions if not a.get("ok", True)]
    last_activity = ""
    last_app = ""
    for t in reversed(timeline):
        if t.get("activity"):
            last_activity = t["activity"]
            last_app = t.get("active_app") or last_app
            break

    ok = not failed_steps and not failed_assertions and not any(
        f["kind"] in ("reboot_reset", "app_fatal", "timeout", "disconnect") for f in findings
    )
    return {
        "ok": ok,
        "findings": findings,
        "heap": heap_info,
        "psram": psram_info,
        "failed_assertions": failed_assertions,
        "failed_steps": [t.get("step") for t in failed_steps],
        "last_activity": last_activity,
        "last_app": last_app,
        "screenshot_hashes": {
            k: hashlib.sha256(Path(v).read_bytes()).hexdigest() if Path(v).is_file() else None
            for k, v in screenshots.items()
        },
    }


def write_reports(out_dir: Path, report: dict[str, Any], timeline: list[dict]) -> None:
    from .redact import redact_obj, redact_text

    out_dir.mkdir(parents=True, exist_ok=True)
    safe_report = redact_obj(report)
    safe_timeline = redact_obj(timeline)
    (out_dir / "report.json").write_text(
        json.dumps(safe_report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (out_dir / "timeline.json").write_text(
        json.dumps(safe_timeline, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    lines = [
        "# m4adb 运行报告",
        "",
        f"- 结果: **{'通过' if safe_report.get('ok') else '失败'}**",
        f"- 最后 Activity: `{safe_report.get('last_activity', '')}`",
        f"- 最后 App: `{safe_report.get('last_app', '')}`",
    ]
    if safe_report.get("heap"):
        h = safe_report["heap"]
        lines.append(f"- 堆: start={h.get('start')} end={h.get('end')} min={h.get('min')}")
    if safe_report.get("findings"):
        lines.append("- 发现:")
        for f in safe_report["findings"]:
            lines.append(f"  - `{f.get('kind')}`: {redact_text(json.dumps(f, ensure_ascii=False))}")
    if safe_report.get("failed_assertions"):
        lines.append("- 断言失败:")
        for a in safe_report["failed_assertions"]:
            lines.append(f"  - {redact_text(str(a))}")
    if safe_report.get("failed_steps"):
        lines.append(f"- 失败步骤: {safe_report['failed_steps']}")
    lines.append("")
    (out_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")
