#!/usr/bin/env python3
"""
Round 3 Lane C — Settings theme minimal contracts

Load themes/murphy-settings/hub.json and l2.json when present;
fail on node type icon/cover/progress; fail on stroke>0;
L2 repeat limit==8; chrome rects match spec §3.

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
    if not hub.is_file() and not l2.is_file():
        pytest.skip(f"themes/murphy-settings/hub.json and l2.json not present — skip until Lane A lands. Checked {hub}, {l2}")
    for p, label in [(hub, "hub.json"), (l2, "l2.json")]:
        if not p.is_file():
            pytest.skip(f"{label} not present at {p} — skip that file")
        theme = _load_json(p)
        all_ns = _all_nodes(theme)
        forbidden = []
        for n in all_ns:
            t = n.get("type", "")
            # Hub: still ban icon/cover/progress. L2 list scroll progress is drawn
            # in SettingsActivity::render (Fengyan list grammar), not as a theme node.
            if t in ("icon", "cover", "progress"):
                forbidden.append((t, n.get("rect"), n.get("binding")))
        assert not forbidden, (
            f"{label} must not contain node type icon/cover/progress per spec §2 (no icons, no borders). "
            f"L2 right-edge scroll bar is an Activity overlay, not a theme progress node. "
            f"Found {forbidden}. Theme: {p}"
        )

def test_settings_theme_no_stroke():
    hub = _find_hub()
    l2 = _find_l2()
    if not hub.is_file() and not l2.is_file():
        pytest.skip(f"both JSON missing — skip stroke check")
    for p, label in [(hub, "hub.json"), (l2, "l2.json")]:
        if not p.is_file():
            continue
        theme = _load_json(p)
        all_ns = _all_nodes(theme)
        bad = []
        for n in all_ns:
            # stroke may be int >0; missing or 0 is OK. Also check "strokeWidth"?
            stroke = n.get("stroke")
            if isinstance(stroke, int) and stroke > 0:
                bad.append((n.get("type"), stroke, n.get("rect")))
            # also reject any explicit stroke string? ignore
        assert not bad, (
            f"{label} must have no stroke>0 per spec §2 (no card/row borders). "
            f"Found stroke>0 nodes: {bad} in {p}. "
            f"Selected tick must be filled round_rect r=0 fill:true with no stroke."
        )

# ---------------------------------------------------------------------------
# 2) L2 repeat limit == 8, Hub limit ==4
# ---------------------------------------------------------------------------

def test_l2_repeat_limit_is_8_and_hub_is_4():
    hub = _find_hub()
    l2 = _find_l2()
    if not l2.is_file():
        pytest.skip(f"{l2} missing — L2 repeat limit 8 cannot be checked until Lane A lands")
    # Check L2
    theme = _load_json(l2)
    nodes = theme.get("nodes", [])
    repeats = [n for n in _all_nodes(theme) if n.get("type") == "repeat"]
    assert repeats, f"L2 theme at {l2} must have a repeat node"
    # Find the main L2 repeat: source should be $page.rows (binding 72) or contain rows
    # Spec §6: $page.rows =72, limit 8, x24 y68 item 432x80 gap4 vertical
    # Accept any repeat whose source string contains "rows" or is $page.rows or limit 8
    # But we enforce limit==8 for the L2 file's repeat(s)
    for r in repeats:
        limit = r.get("limit")
        # Only enforce for vertical repeats near y68 or with limit defined
        if limit is not None:
            assert limit == 8, f"L2 repeat limit must be 8 (spec §3 uses limit=8, kMaxRepeatItems=8). Got {limit} in {r} at {l2}"
    # Also check hub limit 4 if hub present
    if hub.is_file():
        hub_theme = _load_json(hub)
        hub_repeats = [n for n in _all_nodes(hub_theme) if n.get("type") == "repeat"]
        assert hub_repeats, f"hub theme at {hub} must have a repeat node (limit 4)"
        for r in hub_repeats:
            limit = r.get("limit")
            if limit is not None:
                assert limit == 4, f"Hub repeat limit must be 4 per spec §3. Got {limit} in {r} at {hub}"

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
    if not hub.is_file() and not l2.is_file():
        pytest.skip(f"both JSON missing — chrome rects check skipped")

    # Chrome per spec §3: hub vs l2 have different title geometry after round-3/14
    for p, label in [(hub, "hub"), (l2, "l2")]:
        if not p.is_file():
            continue
        theme = _load_json(p)
        nodes = theme.get("nodes", [])
        assert theme.get("screen") == [480, 800], f"{label} screen must be [480,800] per spec §3, got {theme.get('screen')}"

        # Find title text/battery/line at top level (not inside repeat)
        if label == "hub":
            # Hub uses larger header per design-sheet: [24,20,320,32] ui_20_bold
            title_nodes = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 20, 320, 32]]
            assert title_nodes, f"{label} must have title text rect [24,20,320,32] per design-sheet. Nodes: {nodes[:3]} at {p}"
            for tn in title_nodes:
                font = tn.get("font", "")
                assert font == "ui_20_bold", f"{label} title font must be ui_20_bold, got {font} in {tn}"
                txt = tn.get("text", "")
                assert txt == "系统设置" or txt.startswith("$"), f"hub title text must be '系统设置' or $page.title, got {txt!r}"
        else:
            # L2 Hub-matched type: same chrome rect as Hub, ui_24_bold -> kHubCategoryFontId (~24px)
            title_nodes = [n for n in nodes if n.get("type") == "text" and n.get("rect") == [24, 20, 320, 32]]
            assert title_nodes, f"{label} must have title text rect [24,20,320,32] (Hub-matched). Nodes: {nodes[:3]} at {p}"
            for tn in title_nodes:
                font = tn.get("font", "")
                assert font == "ui_24_bold", f"{label} title font must be ui_24_bold (Hub-matched), got {font} in {tn}"
                txt = tn.get("text", "")
                assert txt == "$page.title" or "系统设置" in txt or txt.startswith("$"), f"l2 title text must be $page.title (or literal), got {txt!r}"

        battery_nodes = [n for n in nodes if n.get("type") == "battery" and n.get("rect") == [432, 24, 24, 12]]
        assert battery_nodes, f"{label} must have battery rect [432,24,24,12] per spec §3 at {p}"
        # battery binding should be $system.battery
        for bn in battery_nodes:
            assert bn.get("binding") == "$system.battery", f"battery binding must be $system.battery, got {bn.get('binding')}"

        line_nodes = [n for n in nodes if n.get("type") == "line"]
        assert line_nodes, f"{label} must have a line node (hairline)"
        # Find the chrome line at y=52 x 23-456
        chrome_lines = [ln for ln in line_nodes if ln.get("x") == 23 and ln.get("y") == 52 and ln.get("x2") == 456 and ln.get("y2") == 52]
        assert chrome_lines, (
            f"{label} must have hairline (23,52)-(456,52) width 1 per spec §3. "
            f"Found lines: {line_nodes} at {p}"
        )
        for ln in chrome_lines:
            assert ln.get("width", 1) == 1, f"hairline width must be 1, got {ln.get('width')}"

        # Check that no node exceeds content bottom y≤736 encroachment in an obvious way?
        # Spec says content bottom y≤736 for footer. We don't enforce strict here, but ensure repeat y=68
        repeats = [n for n in nodes if n.get("type") == "repeat"]
        assert repeats, f"{label} must have a repeat"
        for r in repeats:
            assert r.get("x") == 24 and r.get("y") == 68, f"{label} repeat x/y must be 24,68 per spec §3, got x={r.get('x')} y={r.get('y')} in {r}"

    # Hub-specific: repeat geometry (design-sheet values, not old spec §3)
    if hub.is_file():
        theme = _load_json(hub)
        nodes = theme.get("nodes", [])
        r = next((n for n in nodes if n.get("type") == "repeat" and n.get("source") in ("$hub.cards", "$hub.cards", "$hub.cards") or n.get("limit")==4), None)
        # fallback: find repeat with limit 4
        r = next((n for n in nodes if n.get("type") == "repeat" and n.get("limit") == 4), None)
        assert r is not None, f"hub must have repeat limit 4 source $hub.cards at {hub}"
        assert r.get("item_width") == 432 and r.get("item_height") == 100, f"hub repeat item 432x100 per design-sheet, got {r}"
        assert r.get("gap") == 8, f"hub gap 8 (design-sheet), got {r.get('gap')}"
        # children: selected bar [0,12,4,76] fill true visible_if $item.selected, title [24,32,380,36] ui_24_bold $item.title
        children = r.get("children", [])
        # tick
        tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [0, 12, 4, 76]), None)
        assert tick is not None, f"hub repeat must have selected bar [0,12,4,76] per design-sheet, children={children}"
        assert tick.get("fill") is True or tick.get("fill") == 1, f"hub tick must be fill:true, got {tick}"
        assert tick.get("stroke", 0) == 0, f"hub tick stroke must be 0, got {tick.get('stroke')}"
        assert tick.get("r", tick.get("radius", 0)) == 0, f"hub tick r must be 0, got {tick}"
        assert tick.get("visible_if") == "$item.selected", f"hub tick visible_if must be $item.selected, got {tick.get('visible_if')}"
        # title
        title = next((c for c in children if c.get("type") == "text" and c.get("rect") == [24, 32, 380, 36]), None)
        assert title is not None, f"hub repeat must have title [24,32,380,36] per design-sheet, children={children}"
        assert title.get("font") == "ui_24_bold", f"hub title font ui_24_bold, got {title.get('font')}"
        assert title.get("text") == "$item.title" or "$item.title" in str(title.get("text")), f"hub title text must be $item.title, got {title.get('text')}"

    # L2-specific (Hub-matched type: page+row ui_24_bold, section+value ui_18_regular)
    if l2.is_file():
        theme = _load_json(l2)
        nodes = theme.get("nodes", [])
        r = next((n for n in nodes if n.get("type") == "repeat" and n.get("limit") == 8), None)
        assert r is not None, f"l2 must have repeat limit 8 at {l2}"
        assert r.get("item_width") == 432 and r.get("item_height") == 80, f"l2 repeat item 432x80 (footer lock), got {r}"
        assert r.get("gap") == 4, f"l2 gap 4, got {r.get('gap')}"
        children = r.get("children", [])
        tick = next((c for c in children if c.get("type") == "round_rect" and c.get("rect") == [0, 12, 4, 56]), None)
        assert tick is not None, f"l2 repeat must have selected bar [0,12,4,56], children={children}"
        assert tick.get("fill") is True or tick.get("fill") == 1
        assert tick.get("stroke", 0) == 0
        assert tick.get("r", tick.get("radius", 0)) == 0
        assert tick.get("visible_if") == "$item.selected"
        # setting name matches Hub card title face
        name = next((c for c in children if c.get("type") == "text" and c.get("rect") == [20, 22, 240, 36]), None)
        assert name is not None, f"l2 must have setting name [20,22,240,36] Hub-matched, children={children}"
        assert name.get("font") == "ui_24_bold", f"l2 name font ui_24_bold (Hub-matched), got {name.get('font')}"
        assert name.get("visible_if") == "$item.is_row", f"name visible_if $item.is_row, got {name.get('visible_if')}"
        group = next((c for c in children if c.get("type") == "text" and c.get("rect") == [20, 26, 400, 28]), None)
        assert group is not None, f"l2 must have group title [20,26,400,28]"
        assert group.get("font") == "ui_18_regular"
        assert group.get("visible_if") == "$item.is_section"
        value = next((c for c in children if c.get("type") == "text" and c.get("rect") == [280, 24, 140, 32]), None)
        assert value is not None, f"l2 must have value [280,24,140,32]"
        assert value.get("font") == "ui_18_regular"
        assert value.get("align") == "right", f"value align right, got {value.get('align')}"
        assert value.get("visible_if") == "$item.is_row"
        assert value.get("text") == "$item.value"

def test_no_footer_and_no_extra_forbidden():
    # Ensure themes do not contain footer nodes (y>736) and do not reintroduce borders via group rects
    hub = _find_hub()
    l2 = _find_l2()
    if not hub.is_file() and not l2.is_file():
        pytest.skip("no JSON")
    for p in [hub, l2]:
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
