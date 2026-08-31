import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THEME = ROOT / "themes" / "murphy-default" / "theme.json"


def _nodes():
    return json.loads(THEME.read_text(encoding="utf-8"))["nodes"]


def _first(nodes, typ, **match):
    for node in nodes:
        if node.get("type") != typ:
            continue
        if all(node.get(k) == v for k, v in match.items()):
            return node
    raise AssertionError((typ, match))


def test_target_effect_geometry_480x800():
    nodes = _nodes()
    assert _first(nodes, "text", text="Murphy M4")["rect"] == [24, 24, 160, 24]
    assert _first(nodes, "battery")["rect"] == [432, 24, 24, 12]

    divider = _first(nodes, "line", y=52)
    assert [divider["x"], divider["y"], divider["x2"], divider["y2"]] == [23, 52, 456, 52]

    assert _first(nodes, "round_rect")["rect"] == [22, 84, 434, 230]
    assert _first(nodes, "cover", binding="$current.cover")["rect"] == [44, 104, 138, 191]
    assert _first(nodes, "progress")["rect"] == [208, 278, 222, 10]

    recent = _first(nodes, "repeat", source="$recent")
    assert [recent["x"], recent["y"], recent["item_width"], recent["gap"]] == [28, 380, 129, 14]
    assert recent["children"][0]["rect"] == [18, 0, 92, 122]
    assert recent["children"][1]["rect"] == [0, 136, 129, 44]

    # Scope C: verified target effect480 has no long line anywhere y=540..624 — ensure no erroneous divider
    for n in nodes:
        if n.get("type") == "line":
            y = n.get("y")
            x = n.get("x", 0)
            x2 = n.get("x2", 0)
            w = abs(x2 - x) if "x2" in n else 0
            assert not (540 <= y <= 624 and w > 200), f"long divider at y={y} x={x}..{x2} violates no-line 540..624"

    apps = _first(nodes, "repeat", source="$apps")
    assert [apps["x"], apps["y"], apps["item_width"], apps["gap"]] == [24, 636, 100, 10]
    assert apps["children"][0]["rect"] == [18, 0, 62, 64]
    assert apps["children"][1]["rect"] == [0, 78, 100, 18]
