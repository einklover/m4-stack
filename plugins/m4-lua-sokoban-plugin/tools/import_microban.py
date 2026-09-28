#!/usr/bin/env python3
"""Import selected Microban XSB maps for offline M4 curation.

Source format: David W. Skinner's 2000 Microban 155-level text pack.
https://github.com/OMerkel/Sokoban/blob/master/3rdParty/Levels/Microban.txt
This script does NOT ship in the device package and does not copy third-party
code. Retain the original designer's attribution in the plugin README.
"""
import argparse
import hashlib
import re
from collections import deque
from pathlib import Path

DEFAULT_IDS = tuple(range(1, 25))
DIRS = ((0, 1), (0, -1), (1, 0), (-1, 0))

def parse_pack(text):
    pattern = re.compile(r"(?m)^;\s*(\d+)\b[^\n]*$")
    matches = list(pattern.finditer(text))
    result = {}
    for i, match in enumerate(matches):
        region = text[match.end() : (matches[i + 1].start() if i + 1 < len(matches) else len(text))]
        lines = []
        for line in region.splitlines():
            line = line.rstrip("\r")
            if not lines and not line.strip():
                continue
            if not line.strip():
                if lines:
                    break
                continue
            if not re.fullmatch(r"[ #.$@*+]+", line):
                if lines:
                    break
                continue
            lines.append(line)
        if lines:
            result[int(match.group(1))] = lines
    return result

def normalize(lines):
    """Turn XSB's off-board padding into walls; preserve interior floor."""
    width, height = max(map(len, lines)), len(lines)
    grid = [list(line.ljust(width)) for line in lines]
    void = set()
    q = deque()
    for y in range(height):
        for x in (0, width - 1):
            if grid[y][x] == " ":
                q.append((y, x))
    for x in range(width):
        for y in (0, height - 1):
            if grid[y][x] == " ":
                q.append((y, x))
    while q:
        y, x = q.popleft()
        if (y, x) in void or grid[y][x] != " ":
            continue
        void.add((y, x))
        for dy, dx in DIRS:
            ny, nx = y + dy, x + dx
            if 0 <= ny < height and 0 <= nx < width:
                q.append((ny, nx))
    for y, x in void:
        grid[y][x] = "#"
    return ["".join(row) for row in grid]

def import_levels(text, ids=DEFAULT_IDS):
    pack = parse_pack(text)
    missing = set(ids) - pack.keys()
    if missing:
        raise ValueError(f"Missing Microban source IDs: {sorted(missing)}")
    output = []
    for ident in ids:
        rows = normalize(pack[ident])
        if len(rows) > 12 or len(rows[0]) > 15:
            raise ValueError(f"Level {ident}: dimensions exceed the 480px M4 layout ({len(rows[0])}x{len(rows)})")
        output.append(rows)
    return output

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--ids", default=",".join(map(str, DEFAULT_IDS)))
    args = p.parse_args()
    data = args.source.read_bytes()
    ids = tuple(map(int, args.ids.split(",")))
    rows = import_levels(data.decode("utf-8-sig"), ids)
    header = ("-- Microban by David W. Skinner (revised April 2000).\n"
              "-- Imported from OMerkel/Sokoban 3rdParty/Levels/Microban.txt;\n"
              "-- KOReader's Sokoban credits Skinner and describes Microban as public domain.\n"
              "-- These levels are not original M4 designs; include credit in README.\n")
    snippet = header + "Game.LEVELS = {\n" + "".join(
        f"-- Microban {ident}\n[[\n" + "\n".join(level) + "\n]],\n"
        for ident, level in zip(ids, rows)
    ) + "}\n"
    args.output.write_text(snippet, encoding="utf-8")
    print(f"imported={len(rows)} original_source_sha256={hashlib.sha256(data).hexdigest()} "
          f"maxsize={max(len(x[0]) for x in rows)}x{max(len(x) for x in rows)}")

if __name__ == "__main__":
    main()
