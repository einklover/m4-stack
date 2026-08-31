#!/usr/bin/env python3
"""
RED tests for Task 4 — compiler page-specific binding/action manifest.

These tests must be RED before implementation and GREEN after:
- murphy-default preserves known IDs/actions and command order (byte compat)
- settings mock compiles with page-specific IDs/actions
- duplicate IDs, reserved collision, unknown binding/action, boolean-as-int, out-of-range reject
- deterministic compile
- old theme without manifest still works
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = ROOT / "firmware" / "tools" / "compile_home_theme.py"
DEFAULT_THEME = ROOT / "themes" / "murphy-default" / "theme.json"
SETTINGS_MOCK_THEME = ROOT / "themes" / "settings-scene-mock" / "theme.json"

MAGIC = b"M4TH"
HEADER_SIZE = 32
SECTION_SCENE = 6

def _compile(theme_path: Path, out_dir: Path):
    out = out_dir / "out.m4theme"
    cmd = [sys.executable, str(COMPILER), "--theme", str(theme_path), "--out", str(out)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result, out

def _read_header_and_sections(data: bytes):
    magic, ver, hs, total, sw, sh, sc, flags, crc = struct.unpack("<4sHHIHHHHI", data[:24])
    sections = {}
    for i in range(sc):
        typ, f, offset, length, count, scrc = struct.unpack("<IIIIII", data[32 + i*24: 32 + (i+1)*24])
        sections[typ] = {"type": typ, "flags": f, "offset": offset, "length": length, "count": count, "raw": data[offset:offset+length]}
    return {"magic": magic, "version": ver, "header_size": hs, "total_size": total, "section_count": sc}, sections

def _write_theme(tmp: Path, theme_dict: dict, tid="test-manifest"):
    p = tmp / "theme.json"
    p.write_text(json.dumps(theme_dict), encoding="utf-8")
    return p

# Helper to get decode
def _decode_scene(payload: bytes):
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        # reload to pick up changes
        import importlib
        importlib.reload(cht)
        return cht.decode_scene(payload)
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_murphy_default_preserves_known_ids_and_order(tmp_path):
    """Existing murphy-default must compile and decode with preserved legacy IDs and order."""
    result, out = _compile(DEFAULT_THEME, tmp_path)
    assert result.returncode == 0, f"murphy-default compile failed: {result.stderr}"
    data = out.read_bytes()
    hdr, sections = _read_header_and_sections(data)
    assert SECTION_SCENE in sections
    payload = sections[SECTION_SCENE]["raw"]
    # decode via compiler helper
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        cmds = cht.decode_scene(payload)
        # murphy-default has 18 nodes? Check at least that order preserved for first nodes
        types = [c["type"] for c in cmds]
        assert types[0] == "clear"
        assert types[1] == "text"  # Murphy M4
        assert types[2] == "battery"
        # Verify that known bindings map to known numeric IDs in compiler maps
        # We check that compiler still knows $current.title ==11, $recent==20, open_app==3
        assert cht.BINDING_SCENE_MAP["$current.title"] == 11
        assert cht.BINDING_SCENE_MAP["$recent"] == 20
        assert cht.BINDING_SCENE_MAP["$system.battery"] == 1
        assert cht.BINDING_SCENE_MAP["$item.title"] == 32
        assert cht.ACTION_SCENE_MAP["open_app"] == 3
        assert cht.ACTION_SCENE_MAP["open_history"] == 1
        # Ensure command count preserved
        assert sections[SECTION_SCENE]["count"] == len(cmds)
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_old_theme_without_manifest_still_works(tmp_path):
    """Theme without bindings/actions manifest must still compile using legacy vocabulary."""
    nodes = [
        {"type": "clear", "color": "white"},
        {"type": "text", "rect": [30, 28, 150, 24], "text": "$current.title", "font": "ui_16_regular", "align": "left"},
        {"type": "cover", "rect": [52, 129, 110, 180], "radius": 6, "binding": "$current.cover"},
        {"type": "repeat", "source": "$recent", "limit": 2, "x": 42, "y": 405, "item_width": 130, "item_height": 140, "gap": 14, "direction": "horizontal", "children": [
            {"type": "text", "rect": [0, 112, 110, 16], "text": "$item.title", "font": "ui_12_regular"}
        ]},
        {"type": "text", "rect": [30, 382, 100, 20], "text": "全部 >", "font": "ui_14_regular", "action": "open_history"},
    ]
    theme = {"format": 1, "id": "test-legacy", "screen": [480, 800], "nodes": nodes}
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode == 0, f"legacy without manifest should compile: {result.stderr}"

def test_settings_mock_page_specific_bindings_compile(tmp_path):
    """Settings mock with page-specific bindings/actions must compile and use those IDs."""
    # This is the core page-specific manifest test — must be RED before impl
    theme = {
        "format": 1,
        "id": "settings-mock-test",
        "screen": [480, 800],
        "bindings": {
            "$page.settings": 64,
            "$page.status": 65,
            "$item.value": 66,
            "$item.enabled": 67
        },
        "actions": {
            "open_setting": 32,
            "toggle_setting": 33
        },
        "nodes": [
            {"type": "clear", "color": "white"},
            {"type": "text", "rect": [30, 28, 200, 24], "text": "Settings", "font": "ui_16_regular"},
            {"type": "text", "rect": [30, 60, 200, 20], "text": "$page.status", "font": "ui_14_regular"},
            {"type": "text", "rect": [30, 90, 200, 20], "text": "$system.battery", "font": "ui_12_regular"},
            {"type": "repeat", "source": "$recent", "limit": 3, "x": 24, "y": 150, "item_width": 430, "item_height": 60, "gap": 10, "direction": "vertical", "children": [
                {"type": "text", "rect": [0, 0, 200, 18], "text": "$item.title", "font": "ui_14_regular"},
                {"type": "text", "rect": [220, 0, 100, 18], "text": "$item.value", "font": "ui_12_regular"},
                {"type": "icon", "rect": [350, 0, 40, 40], "name": "toggle", "binding": "$item.enabled", "action": "toggle_setting", "action_arg": "$item.id"}
            ]},
            {"type": "text", "rect": [30, 700, 100, 20], "text": "Open", "font": "ui_14_regular", "action": "open_setting"}
        ]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode == 0, f"settings mock with page-specific bindings should compile: {result.stderr}"
    data = out.read_bytes()
    hdr, sections = _read_header_and_sections(data)
    assert SECTION_SCENE in sections
    payload = sections[SECTION_SCENE]["raw"]
    # Decode and verify that payload contains the page-specific numeric IDs, not legacy IDs
    # We check via raw payload bytes containing the binding IDs 64,65,66,67 and action 32,33
    # Since runtime is numeric only, payload should encode those IDs.
    # Simple check: encoded payload should contain those byte values at binding positions
    # We verify via compile_home_theme internal maps merging
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        # For manifest themes, the compiler should have merged common + page-specific
        # Check that decoding preserves order
        cmds = cht.decode_scene(payload)
        types = [c["type"] for c in cmds]
        assert types == ["clear", "text", "text", "text", "repeat", "text"]
        # And that common binding still works ($system.battery ==1)
        assert cht.BINDING_SCENE_MAP["$system.battery"] == 1  # common always 1
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]
    # Also verify payload actually contains page-specific values
    # Look for those bytes in payload (binding IDs are single byte)
    assert 65 in payload  # $page.status
    assert 66 in payload  # $item.value
    assert 67 in payload  # $item.enabled
    assert 32 in payload  # open_setting action
    assert 33 in payload  # toggle_setting action
    # Common binding still present
    assert 1 in payload  # $system.battery

def test_rejects_duplicate_binding_ids(tmp_path):
    theme = {
        "format": 1, "id": "dup-bind", "screen": [480,800],
        "bindings": {"$page.a": 64, "$page.b": 64},
        "nodes": [{"type":"clear","color":"white"}, {"type":"text","rect":[0,0,100,20],"text":"$page.a","font":"ui_16_regular"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "duplicate" in result.stderr.lower()

def test_rejects_duplicate_action_ids(tmp_path):
    theme = {
        "format": 1, "id": "dup-act", "screen": [480,800],
        "actions": {"open_setting": 32, "toggle_setting": 32},
        "nodes": [{"type":"clear","color":"white"}, {"type":"text","rect":[0,0,100,20],"text":"hi","font":"ui_16_regular","action":"open_setting"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "duplicate" in result.stderr.lower()

def test_rejects_reserved_collision_binding(tmp_path):
    # $system.battery is reserved common (ID 1) — manifest must not reuse that ID or name
    theme = {
        "format": 1, "id": "reserved-coll", "screen": [480,800],
        "bindings": {"$page.bad": 1},
        "nodes": [{"type":"clear","color":"white"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "reserved" in result.stderr.lower() or "collision" in result.stderr.lower()

def test_rejects_reserved_collision_binding_name(tmp_path):
    theme = {
        "format": 1, "id": "reserved-name", "screen": [480,800],
        "bindings": {"$system.battery": 64},
        "nodes": [{"type":"clear","color":"white"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "reserved" in result.stderr.lower() or "collision" in result.stderr.lower()

def test_rejects_unknown_binding(tmp_path):
    theme = {
        "format": 1, "id": "unknown-bind", "screen": [480,800],
        "bindings": {"$page.a": 64},
        "nodes": [{"type":"text","rect":[0,0,100,20],"text":"$unknown.not.defined","font":"ui_16_regular"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown binding" in result.stderr.lower()

def test_rejects_unknown_action(tmp_path):
    theme = {
        "format": 1, "id": "unknown-act", "screen": [480,800],
        "actions": {"open_setting": 32},
        "nodes": [{"type":"text","rect":[0,0,100,20],"text":"hi","font":"ui_16_regular","action":"not_defined_action"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown action" in result.stderr.lower()

def test_rejects_boolean_as_int_binding(tmp_path):
    theme = {
        "format": 1, "id": "bool-bind", "screen": [480,800],
        "bindings": {"$page.a": True},
        "nodes": [{"type":"clear","color":"white"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    # should complain about int / bool

def test_rejects_boolean_as_int_action(tmp_path):
    theme = {
        "format": 1, "id": "bool-act", "screen": [480,800],
        "actions": {"open_setting": True},
        "nodes": [{"type":"clear","color":"white"}]
    }
    p = _write_theme(tmp_path, theme)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0

def test_rejects_out_of_range_binding(tmp_path):
    for bad_id in [0, 255, 300, -1]:
        theme = {
            "format": 1, "id": f"oor-bind-{bad_id}", "screen": [480,800],
            "bindings": {"$page.a": bad_id},
            "nodes": [{"type":"clear","color":"white"}]
        }
        sub = tmp_path / f"oor{bad_id}"
        sub.mkdir(parents=True, exist_ok=True)
        p2 = sub / "theme.json"
        p2.write_text(json.dumps(theme), encoding="utf-8")
        result, out = _compile(p2, sub)
        assert result.returncode != 0, f"binding {bad_id} should be rejected but passed: {result.stderr}"

def test_rejects_out_of_range_action(tmp_path):
    for bad_id in [-1, 255, 300]:
        theme = {
            "format": 1, "id": f"oor-act-{bad_id}", "screen": [480,800],
            "actions": {"open_setting": bad_id},
            "nodes": [{"type":"clear","color":"white"}]
        }
        (tmp_path / f"a{bad_id}").mkdir(parents=True, exist_ok=True)
        p2 = tmp_path / f"a{bad_id}" / "theme.json"
        p2.write_text(json.dumps(theme), encoding="utf-8")
        result, out = _compile(p2, tmp_path / f"a{bad_id}")
        assert result.returncode != 0, f"action {bad_id} should be rejected"

def test_deterministic_with_manifest(tmp_path):
    theme = {
        "format": 1, "id": "deterministic", "screen": [480,800],
        "bindings": {"$page.a": 64, "$item.value": 65},
        "actions": {"open_setting": 32},
        "nodes": [
            {"type":"clear","color":"white"},
            {"type":"text","rect":[0,0,100,20],"text":"$page.a","font":"ui_16_regular","action":"open_setting"},
            {"type":"text","rect":[0,30,100,20],"text":"$item.value","font":"ui_12_regular"}
        ]
    }
    p = _write_theme(tmp_path, theme)
    d1 = tmp_path / "d1"
    d1.mkdir(parents=True, exist_ok=True)
    p2 = d1 / "theme.json"
    p2.write_text(json.dumps(theme), encoding="utf-8")
    result1, out1 = _compile(p2, d1)
    d2 = tmp_path / "d2"
    d2.mkdir(parents=True, exist_ok=True)
    import shutil
    shutil.copy(p2, d2 / "theme.json")
    result1, out1 = _compile(d1 / "theme.json", d1)
    result2, out2 = _compile(d2 / "theme.json", d2)
    assert result1.returncode == 0 and result2.returncode == 0, f"{result1.stderr} {result2.stderr}"
    assert out1.read_bytes() == out2.read_bytes()

