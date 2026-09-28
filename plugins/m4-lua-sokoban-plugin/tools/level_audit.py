#!/usr/bin/env python3
"""Offline Sokoban XSB level proof and content-quality audit. Never runs on device."""
import argparse
import heapq
import json
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEVEL_FILE = ROOT / "game.lua"
DIRECTIONS = ((0, -1), (0, 1), (-1, 0), (1, 0))


def read_levels(file=LEVEL_FILE):
    content = Path(file).read_text(encoding="utf-8")
    start = content.index("Game.LEVELS = {")
    end = content.index("\nlocal DIRS", start)
    return [m.splitlines() for m in re.findall(r"\[\[\s*\n(.*?)\n\]\]", content[start:end], re.S)]


def parse(rows):
    rows = [r for r in rows if r]
    if not rows or len({len(r) for r in rows}) != 1:
        raise ValueError("Missing or nonrectangular map")
    h, w = len(rows), len(rows[0])
    terrain, goals, crates, player = set(), set(), set(), []
    for y, row in enumerate(rows):
        for x, char in enumerate(row):
            if char not in "# $.@*+":
                raise ValueError(f"Unknown XSB tile {char!r}")
            pos = y * w + x
            if char != "#":
                terrain.add(pos)
            if char in ".$*+":
                if char in ".*+":
                    goals.add(pos)
                if char in "$*":
                    crates.add(pos)
            if char in "@+":
                player.append(pos)
    if len(player) != 1 or not crates or len(crates) != len(goals):
        raise ValueError("Need exactly one player and equal nonzero crate/goal counts")
    if len(terrain) < 1:
        raise ValueError("No walkable floor")
    return w, h, terrain, frozenset(goals), frozenset(crates), player[0]


def canonical(rows):
    a = tuple(rows)
    options = []
    for _ in range(4):
        options.append(a)
        options.append(tuple(row[::-1] for row in a))
        a = tuple("".join(row[i] for row in a[::-1]) for i in range(len(a[0])))
    return min(options)


def solve(rows, state_cap=200000):
    w, h, floor, goals, start_crates, start_player = parse(rows)
    shift = (-1, 1, -w, w)
    start = (start_player, start_crates)
    best = {start: (0, 0)}
    queue = [(0, 0, 0, start_player, start_crates)]
    tie = 0
    visits = 0
    while queue:
        pushes, steps, _, player, crates = heapq.heappop(queue)
        if best.get((player, crates)) != (pushes, steps):
            continue
        visits += 1
        if crates == goals:
            return {"pushes": pushes, "steps": steps, "explored": visits,
                    "crates": len(crates), "width": w, "height": h}
        if visits >= state_cap:
            raise RuntimeError(f"Search exceeded {state_cap} states")
        y, x = divmod(player, w)
        for (dy, dx), dp in zip(DIRECTIONS, shift):
            ny, nx = y + dy, x + dx
            if not (0 <= nx < w and 0 <= ny < h):
                continue
            neighbor = player + dp
            if neighbor not in floor:
                continue
            if neighbor in crates:
                py, px = ny + dy, nx + dx
                if not (0 <= px < w and 0 <= py < h):
                    continue
                target = neighbor + dp
                if target not in floor or target in crates:
                    continue
                next_crates = frozenset((crates - {neighbor}) | {target})
                cost = (pushes + 1, steps + 1)
            else:
                next_crates = crates
                cost = (pushes, steps + 1)
            key = (neighbor, next_crates)
            if key not in best or cost < best[key]:
                best[key] = cost
                tie += 1
                heapq.heappush(queue, (*cost, tie, neighbor, next_crates))
    raise ValueError("Puzzle is unsolvable")


def audit(levels):
    seen = set()
    metrics = []
    for index, rows in enumerate(levels, 1):
        shape = canonical(rows)
        if shape in seen:
            raise ValueError(f"Level {index} duplicates a prior level under symmetry")
        seen.add(shape)
        item = solve(rows)
        item["level"] = index
        metrics.append(item)
    return metrics


def verify_quality_v2(metrics):
    problems = []
    if len(metrics) < 24:
        problems.append("requires >=24 verified original maps")
    if sum(m["crates"] >= 2 for m in metrics) < 14:
        problems.append("requires >=14 maps with 2+ crates")
    if sum(m["crates"] >= 3 for m in metrics) < 5:
        problems.append("requires >=5 maps with 3+ crates")
    if sum(m["pushes"] >= 6 for m in metrics[8:]) < 6:
        problems.append("requires >=6 non-tutorial maps with optimal 6+ pushes")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", type=Path, default=LEVEL_FILE)
    parser.add_argument("--quality-v2", action="store_true")
    args = parser.parse_args()
    metrics = audit(read_levels(args.file))
    print(json.dumps({
        "level_count": len(metrics),
        "one_push_count": sum(m["pushes"] == 1 for m in metrics),
        "crates_distribution": dict(sorted(Counter(m["crates"] for m in metrics).items())),
        "levels": metrics,
        "quality_v2_unmet": verify_quality_v2(metrics),
    }, ensure_ascii=False, indent=2))
    if args.quality_v2 and verify_quality_v2(metrics):
        raise SystemExit("QUALITY_V2_FAIL: " + "; ".join(verify_quality_v2(metrics)))


if __name__ == "__main__":
    main()
