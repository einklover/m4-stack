#!/usr/bin/env python3
"""
Typography polish regression for murphy-default Home.

Scope: top Murphy M4 logo + homepage font sizes/baselines.
Do NOT change cover/app-icon/progress/divider geometry (locked).
"""
import json
from pathlib import Path

THEME = Path(__file__).resolve().parents[2] / "themes/murphy-default/theme.json"

def _nodes():
    return json.loads(THEME.read_text(encoding="utf-8"))["nodes"]

def _find(nodes, text=None, binding=None, source=None, typ=None):
    for n in nodes:
        if typ is not None and n.get("type") != typ:
            continue
        if text is not None and n.get("text") != text:
            continue
        if binding is not None and n.get("binding") != binding:
            continue
        if source is not None and n.get("source") != source:
            continue
        return n
    raise AssertionError(f"not found typ={typ} text={text} binding={binding} source={source}")

def test_murphy_logo_baseline_and_size():
    n = _find(_nodes(), text="Murphy M4")
    # polished: logo y ~24 (±2) to match ink y 24, keep x 24
    assert 24 <= n["rect"][0] <= 28, f"logo x {n['rect'][0]}"
    assert 22 <= n["rect"][1] <= 26, f"logo y {n['rect'][1]} not polished 22-26"
    assert n["font"] == "ui_22_bold"
    assert 24 <= n["rect"][3] <= 26

def test_hero_status_and_title_baselines():
    nodes = _nodes()
    status = _find(nodes, text="继续阅读")
    # status should be left-aligned at heroTextX 209, y 112 ±2
    assert 207 <= status["rect"][0] <= 211, f"status x {status['rect'][0]} not 209±2"
    assert 110 <= status["rect"][1] <= 114, f"status y {status['rect'][1]} not 112±2"
    assert status["font"] == "ui_16_regular"
    title = _find(nodes, text="$current.title")
    assert 207 <= title["rect"][0] <= 211
    assert 140 <= title["rect"][1] <= 144, f"title y {title['rect'][1]} not 142±2"
    assert title["font"] == "ui_24_bold"
    author = _find(nodes, text="$current.author")
    assert 207 <= author["rect"][0] <= 211
    assert 196 <= author["rect"][1] <= 200, f"author y {author['rect'][1]} not 198±2"
    source = _find(nodes, text="$current.source")
    assert 207 <= source["rect"][0] <= 211
    assert 218 <= source["rect"][1] <= 222, f"source y {source['rect'][1]} not 220±2"
    assert author["font"] == "ui_16_regular"
    assert source["font"] == "ui_16_regular"

def test_progress_text_baseline_left():
    n = _find(_nodes(), text="$home.current.progress_text")
    # should be left at 209,253 ±2, not right 378
    assert 207 <= n["rect"][0] <= 211, f"progress_text x {n['rect'][0]} not left 209±2"
    assert 251 <= n["rect"][1] <= 255, f"progress_text y {n['rect'][1]} not 253±2"
    assert n["font"] == "ui_16_regular"

def test_section_headers_polished():
    for text, ex, ey in [("最近阅读", 28, 347), ("应用", 28, 601)]:
        n = _find(_nodes(), text=text)
        assert 26 <= n["rect"][0] <= 30, f"{text} x {n['rect'][0]}"
        assert ey-2 <= n["rect"][1] <= ey+2, f"{text} y {n['rect'][1]} not {ey}±2"
        assert n["font"] == "ui_20_bold"
    for text, ex, ey in [("全部  >", 401, 347), ("更多  >", 401, 601)]:
        n = _find(_nodes(), text=text)
        assert 399 <= n["rect"][0] <= 403, f"{text} x {n['rect'][0]} not {ex}±2"
        assert ey-2 <= n["rect"][1] <= ey+2
        assert n["font"] == "ui_16_regular"
        assert n["align"] == "left", f"{text} should start at its measured x"

def test_preserved_geometries():
    nodes = _nodes()
    assert _find(nodes, typ="cover", binding="$current.cover")["rect"] == [44,104,138,191]
    assert _find(nodes, typ="progress")["rect"] == [208,278,222,10]
    recent = _find(nodes, typ="repeat", source="$recent")
    assert recent["children"][0]["rect"] == [18,0,92,122]
    assert recent["x"] == 28 and recent["y"] == 380
    apps = _find(nodes, typ="repeat", source="$apps")
    assert apps["children"][0]["rect"] == [18,0,62,64]
    # no long divider 540..624
    for n in nodes:
        if n.get("type")=="line":
            y=n.get("y",0); x=n.get("x",0); x2=n.get("x2",0)
            w=abs(x2-x)
            assert not (540 <= y <= 624 and w>200), f"long divider y={y}"
