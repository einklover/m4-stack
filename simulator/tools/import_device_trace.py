#!/usr/bin/env python3
"""Normalize Murphy M4 serial/SD diagnostics into calibration-friendly JSON.

The importer is deliberately tolerant of mixed logs. Unknown lines are retained
as raw evidence rather than discarded, while known heap/PTSH/reader/panic/EPD
patterns become structured records suitable for simulator calibration.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

KV_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)=(-?(?:0x[0-9A-Fa-f]+|\d+(?:\.\d+)?))")
PTSH_RE = re.compile(r"\[PTSH\]\s+t=(\d+)\s+DIFF\s+event=([A-Za-z_]+)\s+mask=0x([0-9A-Fa-f]+)")
READER_OPEN_RE = re.compile(
    r"\[(\d+)\]\s+open\s+path=(.*?)\s+load_ms=(\d+)\s+ok=(\d+)\s+size=(\d+)\s+free=(\d+)"
)
PANIC_RE = re.compile(r"panic\s+reason=([^\s]+).*?pc=(0x[0-9A-Fa-f]+).*?provider_stage=(0x[0-9A-Fa-f]+)")
TP_RE = re.compile(r"\bTP=(0x[0-9A-Fa-f]+).*?(?:duration|ms)[=: ]+(\d+)\s*ms?", re.I)


def numeric(value: str):
    if value.lower().startswith("0x"):
        return int(value, 16)
    if "." in value:
        return float(value)
    return int(value)


def parse_lines(lines: list[str]) -> dict:
    out = {
        "schema_version": 1,
        "heap_samples": [],
        "page_turn_diffs": [],
        "reader_opens": [],
        "panics": [],
        "epd_timings": [],
        "raw_unclassified": [],
    }
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n")
        classified = False

        m = PTSH_RE.search(line)
        if m:
            out["page_turn_diffs"].append(
                {"line": number, "t_ms": int(m.group(1)), "event": m.group(2),
                 "mask": int(m.group(3), 16), "raw": line}
            )
            classified = True

        m = READER_OPEN_RE.search(line)
        if m:
            out["reader_opens"].append(
                {"line": number, "t_ms": int(m.group(1)), "path": m.group(2),
                 "load_ms": int(m.group(3)), "ok": bool(int(m.group(4))),
                 "size": int(m.group(5)), "free_internal": int(m.group(6))}
            )
            classified = True

        m = PANIC_RE.search(line)
        if m:
            out["panics"].append(
                {"line": number, "reason": m.group(1), "pc": int(m.group(2), 16),
                 "provider_stage": int(m.group(3), 16), "raw": line}
            )
            classified = True

        m = TP_RE.search(line)
        if m:
            out["epd_timings"].append(
                {"line": number, "tp": int(m.group(1), 16), "duration_ms": int(m.group(2)),
                 "raw": line}
            )
            classified = True

        kv = {key: numeric(value) for key, value in KV_RE.findall(line)}
        heap_keys = {
            "free_heap", "min_free_heap", "free_psram", "largest_internal",
            "internal_free", "internal_largest", "psram_free", "psram_largest",
        }
        if heap_keys.intersection(kv):
            sample = {"line": number, "raw": line}
            sample.update(kv)
            out["heap_samples"].append(sample)
            classified = True

        if not classified and line.strip():
            out["raw_unclassified"].append({"line": number, "raw": line})

    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", help="serial or SD log file, or '-' for stdin")
    p.add_argument("-o", "--output", help="output JSON file; stdout when omitted")
    args = p.parse_args(argv)
    try:
        if args.input == "-":
            lines = sys.stdin.readlines()
        else:
            lines = Path(args.input).read_text(encoding="utf-8", errors="replace").splitlines(True)
        parsed = parse_lines(lines)
        text = json.dumps(parsed, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            Path(args.output).write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except OSError as exc:
        print(f"trace import error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
