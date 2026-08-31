#!/usr/bin/env python3
"""
Geometry regression for murphy-default against exact 480x800 effect target.
Measured from https://litter.catbox.moe/4yw67r.png resized to 480x800
(Catbox 971x1619 -> 480x800 Lanczos). Tight tolerances ±2 px, no footer.

This test must FAIL against old Scene preview coordinates and PASS after theme fix.
"""
import json
from pathlib import Path

THEME = Path(__file__).resolve().parents[2] / "themes" / "murphy-default" / "theme.json"

def _rect(node):
    return tuple(node["rect"])

def test_header_and_divider_exact():
    t = json.loads(THEME.read_text())
    nodes = t["nodes"]
    # Header Murphy M4 ink x=26..105 y=24..39 -> rect should tightly enclose
    n = nodes[1]
    assert n["type"] == "text" and n["text"] == "Murphy M4"
    x,y,w,h = n["rect"]
    assert 24 <= x <= 28, f"Murphy M4 x {x} not in [24,28] (target ink 26)"
    assert 22 <= y <= 26, f"Murphy M4 y {y} not in [22,26] (target ink 24)"
    # Battery outer bbox measured 432,24,24,12
    b = nodes[2]
    assert b["type"] == "battery"
    x,y,w,h = b["rect"]
    assert 430 <= x <= 434, f"battery x {x} not 432±2"
    assert 22 <= y <= 26, f"battery y {y} not 24±2"
    assert 22 <= w <= 26, f"battery w {w} not 24±2"
    assert 10 <= h <= 14, f"battery h {h} not 12±2"
    # Header divider measured x=23..455 y=51..52, old was 0,90 full width
    line = nodes[3]
    assert line["type"] == "line"
    assert 22 <= line["x"] <= 24, f"line x {line['x']} not 23±1"
    assert 50 <= line["y"] <= 53, f"line y {line['y']} not 51-52"
    assert 454 <= line["x2"] <= 456, f"line x2 {line['x2']} not 455±1"
    assert line["y"] == line["y2"]

def test_hero_card_exact():
    t = json.loads(THEME.read_text())
    n = t["nodes"][4]
    # Hero outer bbox measured 22,84,434,230 old was 30,108,420,224
    assert n["type"] == "round_rect"
    x,y,w,h = n["rect"]
    assert 20 <= x <= 24, f"hero x {x} not 22±2"
    assert 82 <= y <= 86, f"hero y {y} not 84±2"
    assert 432 <= w <= 436, f"hero w {w} not 434±2"
    assert 228 <= h <= 232, f"hero h {h} not 230±2"
    # Hero cover measured 44,104,138,191 old 52,129,110,180
    c = t["nodes"][5]
    assert c["type"] == "cover"
    x,y,w,h = c["rect"]
    assert 42 <= x <= 46, f"hero cover x {x} not 44±2"
    assert 102 <= y <= 106, f"hero cover y {y} not 104±2"
    assert 136 <= w <= 140, f"hero cover w {w} not 138±2"
    assert 189 <= h <= 193, f"hero cover h {h} not 191±2"

def test_hero_text_and_progress_exact():
    t = json.loads(THEME.read_text())
    nodes = t["nodes"]
    # Status "继续阅读" begins 209,112
    n = nodes[6]
    x,y,w,h = n["rect"]
    assert 207 <= x <= 211, f"status x {x} not 209±2"
    assert 110 <= y <= 114, f"status y {y} not 112±2"
    # Title ink begins 209,142
    n = nodes[7]
    x,y,w,h = n["rect"]
    assert 207 <= x <= 211, f"title x {x} not 209±2"
    assert 140 <= y <= 144, f"title y {y} not 142±2"
    # Author 209,187
    n = nodes[8]
    x,y,w,h = n["rect"]
    assert 207 <= x <= 211
    assert 185 <= y <= 189, f"author y {y} not 187±2"
    # Source 209,209
    n = nodes[9]
    x,y,w,h = n["rect"]
    assert 207 <= x <= 211
    assert 207 <= y <= 211, f"source y {y} not 209±2"
    # Progress left text 209,253
    n = nodes[10]
    x,y,w,h = n["rect"]
    assert 207 <= x <= 211
    assert 251 <= y <= 255, f"progress text y {y} not 253±2"
    # Progress bar outer bbox 208,278,222,10 old 184,325
    n = nodes[11]
    x,y,w,h = n["rect"]
    assert 206 <= x <= 210, f"progress bar x {x} not 208±2"
    assert 276 <= y <= 280, f"progress bar y {y} not 278±2"
    assert 220 <= w <= 224, f"progress bar w {w} not 222±2"
    assert 8 <= h <= 12, f"progress bar h {h} not 10±2"

def test_recent_header_and_repeat_exact():
    t = json.loads(THEME.read_text())
    # Recent header left 28,347 old 30,382
    n = t["nodes"][12]
    x,y,w,h = n["rect"]
    assert 26 <= x <= 30, f"recent left x {x} not 28±2"
    assert 345 <= y <= 349, f"recent left y {y} not 347±2"
    # Recent right "全部 >" 401,347
    n = t["nodes"][13]
    x,y,w,h = n["rect"]
    assert 399 <= x <= 403, f"recent right x {x} not 401±2"
    assert 345 <= y <= 349
    # Recent repeat y=380 old 405, covers 92x122
    n = t["nodes"][14]
    assert n["type"] == "repeat" and n["source"] == "$recent"
    assert 26 <= n["x"] <= 30, f"recent repeat x {n['x']} not 28±2"
    assert 378 <= n["y"] <= 382, f"recent repeat y {n['y']} not 380±2"
    # Children cover 92,122 old 74,106, text y 136 old 112
    c0, c1 = n["children"]
    assert c0["type"] == "cover"
    assert 90 <= c0["rect"][2] <= 94, f"recent cover w {c0['rect'][2]} not 92±2"
    assert 120 <= c0["rect"][3] <= 124, f"recent cover h {c0['rect'][3]} not 122±2"
    assert 134 <= c1["rect"][1] <= 138, f"recent title y {c1['rect'][1]} not 136±2"

def test_apps_header_and_repeat_exact():
    t = json.loads(THEME.read_text())
    n = t["nodes"][15]
    # Apps header 28,601 old 30,580
    x,y,w,h = n["rect"]
    assert 26 <= x <= 30
    assert 599 <= y <= 603, f"apps left y {y} not 601±2"
    n = t["nodes"][16]
    x,y,w,h = n["rect"]
    assert 399 <= x <= 403
    assert 599 <= y <= 603
    # Apps repeat y=636 old 610, outer icon boxes 61x64 old 68x68
    n = t["nodes"][17]
    assert n["type"] == "repeat" and n["source"] == "$apps"
    assert 22 <= n["x"] <= 26, f"apps repeat x {n['x']} not 24±2"
    assert 634 <= n["y"] <= 638, f"apps repeat y {n['y']} not 636±2"
    c0, c1 = n["children"]
    assert c0["type"] == "icon"
    # Icon outer box measured 61x64 (also 62 variants) -> allow 61-64
    assert 60 <= c0["rect"][2] <= 64, f"app icon w {c0['rect'][2]} not 61-64"
    assert 62 <= c0["rect"][3] <= 66, f"app icon h {c0['rect'][3]} not 64±2"
    # Label y 78 relative -> absolute 714, old 76->686
    assert 76 <= c1["rect"][1] <= 80, f"app label y {c1['rect'][1]} not 78±2"

def test_no_footer_nav():
    t = json.loads(THEME.read_text())
    # Ensure no footer nav nodes (bottom nav) — preserve no-footer layout
    text_nodes = [n for n in t["nodes"] if n["type"] == "text" and "rect" in n and n["rect"][1] > 730]
    assert len(text_nodes) == 0, f"footer nav found {text_nodes}, should be 0"
    # Total nodes should be 18 (clear + header 2 + line + hero 6 + recent header2 + repeat + apps header2 + repeat)
    assert len(t["nodes"]) == 18, f"node count {len(t['nodes'])} not 18 (no footer)"
