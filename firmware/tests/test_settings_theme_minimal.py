#!/usr/bin/env python3
"""
Round 3 Lane C — Settings theme minimal contracts

Load themes/murphy-settings/hub.json and l2.json when present;
fail on node type icon/cover/progress; fail on stroke>0;
L2 repeat limit==9 (Advanced v5.1 9-row window); chrome rects match spec §3.

If JSON not present yet, skip/xfail clearly — do not create JSON yourself.

Spec §2 prohibits icons/borders, §3 geometry is locked.

Host: /opt/anaconda3/bin/pytest
"""

import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HUB_JSON = ROOT / "themes" / "murphy-settings" / "hub.json"
L2_JSON = ROOT / "themes" / "murphy-settings" / "l2.json"
ROOT_JSON = ROOT / "themes" / "murphy-settings" / "root.json"
MAINT_JSON = ROOT / "themes" / "murphy-settings" / "maintenance.json"
FRONT_JSON = ROOT / "themes" / "murphy-settings" / "frontlight.json"
KEYS_JSON = ROOT / "themes" / "murphy-settings" / "keys.json"
CHOICE_JSON = ROOT / "themes" / "murphy-settings" / "choice.json"

# Alternative legacy candidate (some impl may use dash vs underscore?)
ALT_HUBS = [
    ROOT / "themes" / "murphy-settings" / "hub.json",
    ROOT / "themes" / "murphy_settings" / "hub.json",
]
ALT_L2S = [
    ROOT / "themes" / "murphy-settings" / "l2.json",
    ROOT / "themes" / "murphy_settings" / "l2.json",
]

def _find_hub():
    for p in ALT_HUBS:
        if p.is_file():
            return p
    return HUB_JSON

def _find_l2():
    for p in ALT_L2S:
        if p.is_file():
            return p
    return L2_JSON

def _load_json(p: Path):
    return json.loads(p.read_text(encoding="utf-8"))

def _collect_nodes_recursive(nodes, out):
    """Flatten nodes including children of repeat/group."""
    for n in nodes:
        out.append(n)
        if isinstance(n.get("children"), list):
            _collect_nodes_recursive(n["children"], out)
        # repeat nodes have children list
        # group nodes have children too
        # Some nodes may have nested repeats

def _all_nodes(theme):
    all_ns = []
    _collect_nodes_recursive(theme.get("nodes", []), all_ns)
    return all_ns

def _has_file(p: Path) -> bool:
    return p.is_file()

# ---------------------------------------------------------------------------
# Skip helpers
# ---------------------------------------------------------------------------

def _skip_if_missing():
    hub = _find_hub()
    l2 = _find_l2()
    if not hub.is_file() and not l2.is_file():
        pytest.skip(f"both {HUB_JSON} and {L2_JSON} missing — RED until Lane A (muse-impl) lands (expected). Not creating JSON.")
    return hub, l2

# ---------------------------------------------------------------------------
# 1) No icon / cover / progress
# ---------------------------------------------------------------------------

def test_settings_theme_no_forbidden_node_types():
    hub = _find_hub()
    l2 = _find_l2()
    files = [(hub, "hub.json"), (l2, "l2.json")]
    if ROOT_JSON.is_file():
        files.append((ROOT_JSON, "root.json"))
    if MAINT_JSON.is_file():
        files.append((MAINT_JSON, "maintenance.json"))
    if FRONT_JSON.is_file():
        files.append((FRONT_JSON, "frontlight.json"))
    if KEYS_JSON.is_file():
        files.append((KEYS_JSON, "keys.json"))
    if CHOICE_JSON.is_file():
        files.append((CHOICE_JSON, "choice.json"))
    if not any(p.is_file() for p, _ in files):
        pytest.skip(f"themes/murphy-settings/hub.json and l2.json not present — skip until Lane A lands. Checked {hub}, {l2}")
    for p, label in files:
        if not p.is_file():
            pytest.skip(f"{label} not present at {p} — skip that file")
        theme = _load_json(p)
        all_ns = _all_nodes(theme)
        forbidden = []
        for n in all_ns:
            t = n.get("type", "")
            # Hub: still ban icon/cover/progress. L2 list scroll progress is drawn
            # in SettingsActivity::render (Fengyan list grammar), not as a theme node.
            # Round-2 item 2: root rows carry one $item.icon glyph each (geometric
            # 1-bit, no bitmap assets); that single shape is allowed, hub stays banned.
            if t in ("cover", "progress"):
                forbidden.append((t, n.get("rect"), n.get("binding")))
            elif t == "icon":
                if not (label == "root.json" and n.get("binding") == "$item.icon"
                        and n.get("visible_if") == "$item.is_row"):
                    forbidden.append((t, n.get("rect"), n.get("binding")))
        assert not forbidden, (
            f"{label} must not contain node type cover/progress per spec §2 (no borders). "
            f"Only root.json row $item.icon glyphs are allowed (round-2 item 2). "
            f"L2 right-edge scroll bar is an Activity overlay, not a theme progress node. "
            f"Found {forbidden}. Theme: {p}"
        )

def test_settings_theme_no_stroke():
    # Border rule (grouped-root revision): no stroke>1 anywhere; 1px outlines
    # only as top-level group cards (round_rect r=8, no fill); repeat children
    # stay stroke-free; no filled rects or filled r>0 round_rects (no solid cards).
    hub = _find_hub()
    l2 = _find_l2()
    files = [(hub, "hub.json"), (l2, "l2.json")]
    if ROOT_JSON.is_file():
        files.append((ROOT_JSON, "root.json"))
    if MAINT_JSON.is_file():
        files.append((MAINT_JSON, "maintenance.json"))
    if FRONT_JSON.is_file():
        files.append((FRONT_JSON, "frontlight.json"))
    if KEYS_JSON.is_file():
        files.append((KEYS_JSON, "keys.json"))
    if CHOICE_JSON.is_file():
        files.append((CHOICE_JSON, "choice.json"))
    if not any(p.is_file() for p, _ in files):
        pytest.skip(f"both JSON missing — skip stroke check")
    for p, label in files:
        if not p.is_file():
            continue
        theme = _load_json(p)
        nodes = theme.get("nodes", [])
        bad = []
        for n in _all_nodes(theme):
            stroke = n.get("stroke")
            if isinstance(stroke, int) and stroke > 1:
                bad.append((n.get("type"), stroke, n.get("rect")))
            if n.get("type") == "rect" and n.get("fill") is True:
                bad.append(("rect-fill", n.get("rect")))
            if n.get("type") == "round_rect" and n.get("fill") is True and n.get("r", 0) != 0:
                # The $item.selected outline is dotted dressing, not a solid card.
                rect = n.get("rect") or []
                if not (n.get("stroke") == 1 and len(rect) == 4 and rect[2] > rect[3]
                        and n.get("visible_if") == "$item.selected"):
                    bad.append(("round_rect-fill-r", n.get("r"), n.get("rect")))
        for n in nodes:
            if n.get("type") == "round_rect" and isinstance(n.get("stroke"), int) and n["stroke"] == 1:
                # v5.1 root cards use r=11; every other page stays r=8 (root-only round).
                ok_radius = (n.get("r") == 11) if label == "root.json" else (n.get("r") == 8)
                if n.get("fill") is True or not ok_radius:
                    bad.append(("card-shape", n.get("rect")))
        for r in [n for n in _all_nodes(theme) if n.get("type") == "repeat"]:
            for c in r.get("children", []):
                if "stroke" not in c:
                    continue
                # Only radio circles (1px outline, square, r == w/2, no fill),
                # the selected outline (1px, non-square, r>0, fill:true for the
                # sparse dotted dressing, gated on $item.selected), and the
                # Root-only stipple field (same shape with r==0: no outline,
                # the card already draws the border).
                rect = c.get("rect") or []
                ok_circle = (c.get("type") == "round_rect" and c.get("stroke") == 1
                             and c.get("fill") is not True and len(rect) == 4
                             and rect[2] == rect[3] and c.get("r", -1) == rect[2] // 2)
                ok_outline = (c.get("type") == "round_rect" and c.get("stroke") == 1
                              and c.get("fill") is True and len(rect) == 4
                              and rect[2] > rect[3] and c.get("r", 0) > 0
                              and c.get("visible_if") == "$item.selected")
                ok_stipple = (label == "root.json" and c.get("type") == "round_rect"
                              and c.get("stroke") == 1 and c.get("fill") is True
                              and len(rect) == 4 and rect[2] > rect[3]
                              and c.get("r", -1) == 0
                              and c.get("visible_if") == "$item.selected")
                if not (ok_circle or ok_outline or ok_stipple):
                    bad.append(("repeat-child-stroke", c.get("type"), c.get("rect")))
        assert not bad, (
            f"{label} border rule violated (1px group-card outlines, radio circles, "
            f"one $item.selected row outline, or the Root-only stipple field; no solid cards). "
            f"Found: {bad} in {p}. "
            f"Selected emphasis must be a stroked non-square round_rect with fill:true "
            f"(r>0 outline, or r==0 Root-only stipple field)."
        )

# ---------------------------------------------------------------------------
# 2) L2 repeat limit == 9 (Advanced v5.1 9-row window; kMaxRepeatItems=9);
# Hub 4-card theme is retired
# ---------------------------------------------------------------------------

def test_l2_repeat_limit_is_9_and_hub_is_retired():
    hub = _find_hub()
    l2 = _find_l2()
    if not l2.is_file():
        pytest.skip(f"{l2} missing — L2 repeat limit 9 cannot be checked")
    theme = _load_json(l2)
    repeats = [n for n in _all_nodes(theme) if n.get("type") == "repeat"]
    assert repeats, f"L2 theme at {l2} must have a repeat node"
    for r in repeats:
        limit = r.get("limit")
        if limit is not None:
            assert limit == 9, (
                f"L2 repeat limit must be 9 (Advanced v5.1 9-row window, kMaxRepeatItems=9). Got {limit} in {r} at {l2}"
            )
    if hub.is_file():
        hub_theme = _load_json(hub)
        hub_repeats = [n for n in _all_nodes(hub_theme) if n.get("type") == "repeat"]
        for r in hub_repeats:
            limit = r.get("limit")
            assert limit != 4, f"retired Hub theme must not keep 4-card repeat, got {r} at {hub}"
            if limit is not None:
                assert limit == 9, f"if Hub JSON still has a repeat, limit must be 9 not {limit}"

# ---------------------------------------------------------------------------
# 2b) Advanced v5.1: 9-row window + navigates-gated chevron
# (layout-v5.1-advanced: G0[0-4] + G1[5-8] = 9 true rows at 52px;
# chevron only on rows whose tap leaves the list)
# ---------------------------------------------------------------------------

def test_l2_v51_window_capacity_9_and_navigates_chevron():
    l2 = _find_l2()
    if not l2.is_file():
        pytest.skip(f"{l2} missing — Advanced v5.1 window/chevron cannot be checked")
    theme = _load_json(l2)
    bindings = theme.get("bindings", {})
    assert bindings.get("$item.navigates") == 78, (
        f"l2 bindings must carry $item.navigates=78 (SettingsSceneModel numeric ABI), got {bindings}"
    )
    nodes = theme.get("nodes", [])
    repeats = [n for n in nodes if n.get("type") == "repeat"]
    assert repeats, f"l2 must have a repeat at {l2}"
    r = repeats[0]
    assert r.get("limit") == 9, (
        f"l2 repeat limit must be 9 (Advanced v5.1 9-row window), got {r.get('limit')}"
    )
    children = r.get("children", [])
    chev = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
            if c.get("type") == "line" and c.get("width") == 2 and c.get("color") == "black"}
    assert chev == {(418, 21, 423, 26), (423, 26, 418, 31)}, \
        f"l2 repeat must carry exactly the 2-stroke > chevron, got {chev}"
    for c in children:
        if c.get("type") == "line" and (c["x"], c["y"], c["x2"], c["y2"]) in chev:
            assert c.get("visible_if") == "$item.navigates", (
                f"chevron strokes must gate on $item.navigates (toggle rows flip in place), got {c}"
            )
    name = next((c for c in children if c.get("type") == "text" and c.get("rect") == [18, 18, 200, 20]), None)
    assert name is not None and name.get("visible_if") == "$item.is_row"
    value = next((c for c in children if c.get("type") == "text" and c.get("rect") == [222, 20, 174, 18]), None)
    assert value is not None and value.get("visible_if") == "$item.is_row"

# ---------------------------------------------------------------------------
# 3) Chrome rects match spec §3
# ---------------------------------------------------------------------------

def _assert_rect(node, expected, ctx):
    rect = node.get("rect")
    assert rect == expected, f"{ctx} rect must be {expected}, got {rect} in node {node}"

def _find_nodes_by_type(nodes, t):
    return [n for n in nodes if n.get("type") == t]

def test_settings_chrome_rects_match_spec():
    hub = _find_hub()
    l2 = _find_l2()
    if not l2.is_file():
        pytest.skip(f"{l2} missing — chrome rects check skipped")

    if hub.is_file():
        hub_theme = _load_json(hub)
        assert hub_theme.get("screen") == [480, 800]
        hub_repeats = [n for n in _all_nodes(hub_theme) if n.get("type") == "repeat"]
        assert not any(r.get("limit") == 4 for r in hub_repeats), "Hub 4-card repeat must be retired"

    theme = _load_json(l2)
    nodes = theme.get("nodes", [])
    assert theme.get("screen") == [480, 800], f"l2 screen must be [480,800] per spec §3, got {theme.get('screen')}"

    title_nodes = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 48, 300, 32]]
    assert title_nodes, f"l2 must have title text rect [24,48,300,32] per spec §3. Nodes: {nodes[:3]} at {l2}"
    for tn in title_nodes:
        font = tn.get("font", "")
        assert font == "ui_24_bold", f"l2 title font must be ui_24_bold, got {font} in {tn}"
        txt = tn.get("text", "")
        assert txt == "$page.title" or txt == "设置" or txt.startswith("$"), f"l2 title must be $page.title, got {txt!r}"

    battery_nodes = [n for n in nodes if n.get("type") == "battery" and n.get("rect") == [431, 18, 22, 10]]
    assert battery_nodes, f"l2 must have battery rect [431,18,22,10] per spec §3 at {l2}"
    for bn in battery_nodes:
        assert bn.get("binding") == "$system.battery", f"battery binding must be $system.battery, got {bn.get('binding')}"

    line_nodes = [n for n in nodes if n.get("type") == "line"]
    assert line_nodes, "l2 must have a line node (hairline)"
    chrome_lines = [ln for ln in line_nodes if ln.get("x") == 22 and ln.get("y") == 91 and ln.get("x2") == 458 and ln.get("y2") == 91]
    assert chrome_lines, f"l2 must have hairline (22,91)-(458,91) width 1. Found lines: {line_nodes}"
    for ln in chrome_lines:
        assert ln.get("width", 1) == 1, f"hairline width must be 1, got {ln.get('width')}"

    repeats = [n for n in nodes if n.get("type") == "repeat"]
    assert repeats, "l2 must have a repeat"
    for r in repeats:
        assert r.get("x") == 22 and r.get("y") == 126, f"l2 repeat x/y must be 22,126 per spec §3, got x={r.get('x')} y={r.get('y')}"

    cards = [n.get("rect") for n in nodes if n.get("type") == "round_rect" and n.get("stroke") == 1]
    assert cards == [], f"l2 flat rows breathe on dividers (no window card), got {cards}"

    r = next((n for n in nodes if n.get("type") == "repeat" and n.get("limit") == 9), None)
    assert r is not None, f"l2 must have repeat limit 9 at {l2}"
    assert r.get("item_width") == 436 and r.get("item_height") == 52, f"l2 repeat item 436x52, got {r}"
    assert r.get("gap") == 0, f"l2 gap 0, got {r.get('gap')}"
    children = r.get("children", [])
    tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [4, 4, 428, 44]), None)
    assert tick is not None, f"l2 repeat must have selected outline [4,4,428,44], children={children}"
    assert tick.get("stroke") == 1
    assert tick.get("r", tick.get("radius", 0)) == 9
    assert tick.get("fill") is True
    assert tick.get("visible_if") == "$item.selected"
    name = next((c for c in children if c.get("type") == "text" and c.get("rect") == [18, 18, 200, 20]), None)
    assert name is not None, f"l2 must have setting name [18,18,200,20], children={children}"
    assert name.get("font") == "ui_18_regular", f"l2 name font ui_18_regular, got {name.get('font')}"
    assert name.get("visible_if") == "$item.is_row"
    assert not any(c.get("visible_if") == "$item.is_section" for c in children), "theme must not paint section rows"
    value = next((c for c in children if c.get("type") == "text" and c.get("rect") == [222, 20, 174, 18]), None)
    assert value is not None, f"l2 must have value [222,20,174,18] (chevron reserve)"
    assert value.get("font") == "ui_14_regular"
    assert value.get("align") == "right", f"value align right, got {value.get('align')}"
    assert value.get("visible_if") == "$item.is_row"
    assert value.get("text") == "$item.value"
    chev = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
            if c.get("type") == "line" and c.get("visible_if") == "$item.navigates"
            and c.get("width") == 2 and c.get("color") == "black"}
    assert chev == {(418, 21, 423, 26), (423, 26, 418, 31)}, \
        f"l2 repeat must carry exactly the 2-stroke > chevron gated on $item.navigates, got {chev}"

def test_root_grouped_chrome_matches_spec():
    # Root-only grouped package: fixed 8-row catalog in 4 visual groups.
    # Groups are presentation-only: flat order/identity/window math never change.
    if not ROOT_JSON.is_file():
        pytest.skip(f"{ROOT_JSON} missing — root grouped chrome cannot be checked")
    theme = _load_json(ROOT_JSON)
    nodes = theme.get("nodes", [])
    assert theme.get("screen") == [480, 800]

    title = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 48, 300, 32]]
    assert title and title[0].get("font") == "ui_24_bold" and title[0].get("text") == "$page.title"
    brand = [n for n in nodes if n.get("type") == "text" and n.get("text") == "Murphy M4"]
    assert brand, "root must carry the Murphy M4 brand chrome"
    assert not [n for n in nodes if n.get("type") == "text" and n.get("text") == "简洁、克制、易读"], \
        "v5.1 root header carries no subtitle line"
    hair = [n for n in nodes if n.get("type") == "line" and (n.get("x"), n.get("y"), n.get("x2"), n.get("y2")) == (22, 91, 458, 91)]
    assert hair and hair[0].get("width", 1) == 1, "root hairline must be (22,91)-(458,91) width 1"

    labels = [n.get("text") for n in nodes
              if n.get("type") == "text" and n.get("font") == "ui_14_bold" and n.get("rect") in (
                  [24, 105, 300, 20], [24, 205, 300, 20], [24, 363, 300, 20], [24, 579, 300, 20])]
    assert labels == ["连接与设备", "阅读与显示", "设备与操作", "系统与其他"], f"root group labels wrong: {labels}"

    cards = [n.get("rect") for n in nodes if n.get("type") == "round_rect" and n.get("stroke") == 1]
    assert cards == [[22, 130, 436, 58], [22, 230, 436, 116], [22, 388, 436, 174], [22, 604, 436, 116]], \
        f"root group cards wrong: {cards}"
    dividers = {(n.get("x"), n.get("y"), n.get("x2"), n.get("y2")) for n in nodes
                if n.get("type") == "line" and n.get("width", 1) == 1
                and (n.get("x"), n.get("x2")) == (58, 446)}
    assert dividers == {(58, 288, 446, 288), (58, 446, 446, 446), (58, 504, 446, 504),
                        (58, 662, 446, 662)}, f"root group dividers wrong: {dividers}"

    repeats = [n for n in nodes if n.get("type") == "repeat"]
    assert [(r.get("source"), r.get("limit"), r.get("x"), r.get("y"),
             r.get("item_width"), r.get("item_height"), r.get("gap")) for r in repeats] == [
        ("$page.rows0", 1, 24, 130, 432, 58, 0),
        ("$page.rows1", 2, 24, 230, 432, 58, 0),
        ("$page.rows2", 3, 24, 388, 432, 58, 0),
        ("$page.rows3", 2, 24, 604, 432, 58, 0),
    ], f"root group repeats wrong: {repeats}"
    assert sum(r.get("limit") for r in repeats) == 8, "root group limits must sum to the 8 catalog rows"
    # Per-group v4 row geometry (item coords): icon box/label baseline/value
    # anchor/chevron column. Baselines equal SVG values up to 2px (repeat
    # shared children cannot express per-row 1px optical tweaks).
    v4_rows = {
        "$page.rows0": {"icon": [15, 18, 18, 18], "name": [46, 18, 170, 20],
                        "value": [220, 22, 174, 18],
                        "chev": {(415, 24, 421, 30), (421, 30, 415, 36)}},
        "$page.rows1": {"icon": [15, 15, 18, 18], "name": [46, 18, 170, 20],
                        "value": [220, 22, 174, 18],
                        "chev": {(415, 24, 421, 30), (421, 30, 415, 36)}},
        "$page.rows2": {"icon": [15, 15, 18, 18], "name": [46, 18, 170, 20],
                        "value": [220, 22, 174, 18],
                        "chev": {(415, 24, 421, 30), (421, 30, 415, 36)}},
        "$page.rows3": {"icon": [15, 15, 18, 18], "name": [46, 18, 170, 20],
                        "value": [220, 22, 174, 18],
                        "chev": {(415, 24, 421, 30), (421, 30, 415, 36)}},
    }
    for r in repeats:
        exp = v4_rows[r.get("source")]
        children = r.get("children", [])
        tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [2, 4, 428, 50]), None)
        assert tick is not None and tick.get("stroke") == 1 and tick.get("r", -1) == 0 \
            and tick.get("fill") is True \
            and tick.get("visible_if") == "$item.selected", f"root rows keep the selected stipple field, got {children}"
        icon = next((c for c in children if c.get("type") == "icon"), None)
        assert icon is not None and icon.get("rect") == exp["icon"] \
            and icon.get("binding") == "$item.icon", f"root row icon wrong: {children}"
        name = next((c for c in children if c.get("type") == "text" and c.get("rect") == exp["name"]), None)
        assert name is not None and name.get("font") == "ui_18_regular" \
            and name.get("visible_if") == "$item.is_row", f"root row name wrong: {children}"
        value = next((c for c in children if c.get("type") == "text" and c.get("rect") == exp["value"]), None)
        assert value is not None and value.get("font") == "ui_14_regular" \
            and value.get("align") == "right" and value.get("text") == "$item.value", \
            f"root row value wrong: {children}"
        chev = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
                if c.get("type") == "line" and c.get("visible_if") == "$item.is_row"
                and c.get("width") == 2}
        assert chev == exp["chev"], f"root chevron wrong: {chev}"
        assert not any(c.get("visible_if") == "$item.is_section" for c in children)

    # Manifest ABI: group row sources must be the reserved-free IDs 80..83.
    bindings = theme.get("bindings", {})
    assert bindings.get("$page.title") == 70 and bindings.get("$item.value") == 74 \
        and bindings.get("$item.selected") == 75 and bindings.get("$item.is_row") == 77
    assert [bindings.get(f"$page.rows{i}") for i in range(4)] == [80, 81, 82, 83]
    assert theme.get("actions", {}).get("activate_setting") == 41


def test_maintenance_grouped_chrome_matches_spec():
    # Maintenance child page: same header language, one static group
    # (label + card) over the flat 4-row child list. No model change:
    # ChildList order/count/identity math never see the card.
    if not MAINT_JSON.is_file():
        pytest.skip(f"{MAINT_JSON} missing — maintenance chrome cannot be checked")
    theme = _load_json(MAINT_JSON)
    nodes = theme.get("nodes", [])
    assert theme.get("screen") == [480, 800]

    title = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 40, 300, 32]]
    assert title and title[0].get("font") == "ui_24_bold" and title[0].get("text") == "$page.title"
    brand = [n for n in nodes if n.get("type") == "text" and n.get("text") == "Murphy M4"]
    assert brand, "maintenance must carry the Murphy M4 brand chrome"
    hair = [n for n in nodes if n.get("type") == "line" and (n.get("x"), n.get("y"), n.get("x2"), n.get("y2")) == (23, 100, 456, 100)]
    assert hair and hair[0].get("width", 1) == 1
    label = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 108, 300, 20]]
    assert label and label[0].get("text") == "设备维护" and label[0].get("font") == "ui_16_bold"
    cards = [n.get("rect") for n in nodes if n.get("type") == "round_rect" and n.get("stroke") == 1]
    assert cards == [[22, 132, 436, 312]], f"maintenance card wrong: {cards}"

    repeats = [n for n in nodes if n.get("type") == "repeat"]
    assert len(repeats) == 1, f"maintenance must have exactly one flat repeat, got {repeats}"
    r = repeats[0]
    assert (r.get("source"), r.get("limit"), r.get("x"), r.get("y"),
            r.get("item_width"), r.get("item_height"), r.get("gap")) == \
        ("$page.rows", 4, 24, 138, 432, 72, 4), f"maintenance repeat wrong: {r}"
    children = r.get("children", [])
    tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [2, 5, 428, 62]), None)
    assert tick is not None and tick.get("stroke") == 1 and tick.get("r", 0) == 8 \
        and tick.get("fill") is True and tick.get("visible_if") == "$item.selected"
    name = next((c for c in children if c.get("type") == "text" and c.get("rect") == [20, 13, 232, 22]), None)
    assert name is not None and name.get("font") == "ui_16_bold" and name.get("visible_if") == "$item.is_row"
    value = next((c for c in children if c.get("type") == "text" and c.get("rect") == [260, 17, 148, 18]), None)
    assert value is not None and value.get("font") == "ui_14_regular" \
        and value.get("align") == "right" and value.get("text") == "$item.value"
    chev = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
            if c.get("type") == "line" and c.get("visible_if") == "$item.is_row"
            and c.get("width") == 2}
    assert chev == {(416, 27, 426, 36), (426, 36, 416, 45)}, f"maintenance chevron wrong: {chev}"
    bindings = theme.get("bindings", {})
    assert bindings.get("$page.title") == 70 and bindings.get("$page.rows") == 72 \
        and bindings.get("$item.value") == 74 and bindings.get("$item.selected") == 75 \
        and bindings.get("$item.is_row") == 77
    assert theme.get("actions", {}).get("activate_setting") == 41


def _assert_grouped_child_chrome(path, label_text, card_rect, limit, radio=False):
    theme = _load_json(path)
    nodes = theme.get("nodes", [])
    assert theme.get("screen") == [480, 800]
    title = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 40, 300, 32]]
    assert title and title[0].get("font") == "ui_24_bold" and title[0].get("text") == "$page.title"
    brand = [n for n in nodes if n.get("type") == "text" and n.get("text") == "Murphy M4"]
    assert brand, f"{path} must carry the Murphy M4 brand chrome"
    hair = [n for n in nodes if n.get("type") == "line" and (n.get("x"), n.get("y"), n.get("x2"), n.get("y2")) == (23, 100, 456, 100)]
    assert hair and hair[0].get("width", 1) == 1
    if label_text is not None:
        label = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 108, 300, 20]]
        assert label and label[0].get("text") == label_text and label[0].get("font") == "ui_16_bold"
        cards = [n.get("rect") for n in nodes if n.get("type") == "round_rect" and n.get("stroke") == 1]
        assert cards == [card_rect], f"{path} card wrong: {cards}"
    repeats = [n for n in nodes if n.get("type") == "repeat"]
    assert len(repeats) == 1
    r = repeats[0]
    assert r.get("source") == "$page.rows" and r.get("limit") == limit
    assert (r.get("x"), r.get("y"), r.get("item_width"), r.get("item_height"), r.get("gap")) == \
        (24, 138 if label_text is not None else 112, 432, 72, 4), f"{path} repeat wrong: {r}"
    children = r.get("children", [])
    if radio:
        assert not any(c.get("type") == "round_rect" and c.get("rect") == [0, 11, 8, 50] for c in children), \
            f"{path} choice rows must not carry the selected bar"
        outer = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [30, 19, 34, 34]), None)
        assert outer is not None and outer.get("stroke") == 1 and outer.get("r") == 17 \
            and outer.get("visible_if") == "$item.is_row", f"{path} radio circle wrong: {children}"
        assert not any(c.get("type") == "round_rect" and c.get("rect") == [35, 24, 24, 24] for c in children), \
            f"{path} inner ring was replaced by the geometric hook: {children}"
        assert not any(c.get("type") == "line" and c.get("width") == 2 for c in children), \
            f"{path} choice rows must not carry a chevron"
        outline = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [2, 5, 428, 62]), None)
        assert outline is not None and outline.get("stroke") == 1 and outline.get("r", 0) == 8 \
            and outline.get("fill") is True and outline.get("visible_if") == "$item.selected", \
            f"{path} choice rows carry the restrained selected outline, got {children}"
    else:
        tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [2, 5, 428, 62]), None)
        assert tick is not None and tick.get("stroke") == 1 and tick.get("r", 0) == 8 \
            and tick.get("fill") is True and tick.get("visible_if") == "$item.selected"
        chev = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
                if c.get("type") == "line" and c.get("visible_if") == "$item.is_row"
                and c.get("width") == 2}
        assert chev == {(416, 27, 426, 36), (426, 36, 416, 45)}, f"{path} chevron wrong: {chev}"
    name_w = 172 if radio else 232
    name_x = 76 if radio else 20
    name = next((c for c in children if c.get("type") == "text" and c.get("rect") == [name_x, 13, name_w, 22]), None)
    assert name is not None and name.get("font") == "ui_16_bold" and name.get("visible_if") == "$item.is_row"
    if radio:
        # No ✓ text node (U+2713 is outside the device UI charset): the checked
        # cursor row carries a geometric hook instead; model still sets ✓.
        assert not any(c.get("type") == "text" and c.get("text") == "$item.value" for c in children), \
            f"{path} choice rows must not carry a ✓ text node, got {children}"
        hook = {(c["x"], c["y"], c["x2"], c["y2"]) for c in children
                if c.get("type") == "line" and c.get("visible_if") == "$item.selected"
                and c.get("width") == 3 and c.get("color") == "black"}
        assert hook == {(392, 28, 398, 34), (398, 34, 410, 22)}, f"{path} check hook wrong: {hook}"
        return
    value = next((c for c in children if c.get("type") == "text" and c.get("rect") == [260, 17, 148, 18]), None)
    assert value is not None and value.get("font") == "ui_14_regular" \
        and value.get("align") == "right" and value.get("text") == "$item.value"
    bindings = theme.get("bindings", {})
    assert bindings.get("$page.title") == 70 and bindings.get("$page.rows") == 72 \
        and bindings.get("$item.value") == 74 and bindings.get("$item.selected") == 75 \
        and bindings.get("$item.is_row") == 77
    assert theme.get("actions", {}).get("activate_setting") == 41


def test_frontlight_grouped_chrome_matches_spec():
    if not FRONT_JSON.is_file():
        pytest.skip("frontlight.json missing")
    _assert_grouped_child_chrome(FRONT_JSON, "亮度与色温", [22, 132, 436, 160], 2)


def test_keys_grouped_chrome_matches_spec():
    if not KEYS_JSON.is_file():
        pytest.skip("keys.json missing")
    _assert_grouped_child_chrome(KEYS_JSON, "按键功能", [22, 132, 436, 464], 6)


def test_choice_radio_chrome_matches_spec():
    if not CHOICE_JSON.is_file():
        pytest.skip("choice.json missing")
    _assert_grouped_child_chrome(CHOICE_JSON, None, None, 8, radio=True)


def test_no_footer_and_no_extra_forbidden():
    # Ensure themes do not contain footer nodes (y>736) and do not reintroduce borders via group rects
    hub = _find_hub()
    l2 = _find_l2()
    files = [hub, l2]
    if ROOT_JSON.is_file():
        files.append(ROOT_JSON)
    if MAINT_JSON.is_file():
        files.append(MAINT_JSON)
    if FRONT_JSON.is_file():
        files.append(FRONT_JSON)
    if KEYS_JSON.is_file():
        files.append(KEYS_JSON)
    if CHOICE_JSON.is_file():
        files.append(CHOICE_JSON)
    if not any(p.is_file() for p in files):
        pytest.skip("no JSON")
    for p in files:
        if not p.is_file():
            continue
        theme = _load_json(p)
        all_ns = _all_nodes(theme)
        # No node should be placed at y>736 that looks like footer (except maybe hints which are not in theme)
        # Spec §3: Scene 不画 footer, y≤736. We check that no text node at y>=740 claims footer
        footer_like = [n for n in all_ns if n.get("type") == "text" and isinstance(n.get("rect"), list) and len(n["rect"]) == 4 and n["rect"][1] >= 740]
        assert not footer_like, f"{p} must not contain footer text at y>=740 (footer is overlay GUI.drawButtonHints). Found {footer_like}"
        # Also ensure no chevron/switch graphics via icon name containing chevron/switch
        for n in all_ns:
            name = n.get("name", "") or n.get("icon", "")
            if isinstance(name, str) and any(k in name.lower() for k in ["chevron", "switch", "toggle"]):
                assert False, f"{p} must not contain chevron/switch graphics per spec §2, found {n}"
