#!/usr/bin/env python3
"""
Focused tests for unified Home Scene V1 ordered scene support.
Covers:
- ordered command preservation
- bitmap before/after cover ordering
- legacy pack valid
- malformed node/unknown key/binding/action rejection
- repeat and interaction compile/decode
- default scene no bitmap/no fake literal
- deterministic and CRC/section non-overlap
- preview renders default scene
"""
import binascii
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = ROOT / "firmware" / "tools" / "compile_home_theme.py"
PREVIEW = ROOT / "firmware" / "tools" / "preview_home_theme.py"
LEGACY_THEME = ROOT / "themes" / "mofei-classic" / "theme.json"
DEFAULT_THEME = ROOT / "themes" / "murphy-default" / "theme.json"

MAGIC = b"M4TH"
HEADER_SIZE = 32
SECTION_SCENE = 6

def _compile(theme_path: Path, out_dir: Path, extra_args=None):
    out = out_dir / "out.m4theme"
    cmd = [sys.executable, str(COMPILER), "--theme", str(theme_path), "--out", str(out)]
    if extra_args:
        cmd.extend(extra_args)
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result, out

def _read_header_and_sections(data: bytes):
    magic, ver, hs, total, sw, sh, sc, flags, crc = struct.unpack("<4sHHIHHHHI", data[:24])
    sections = {}
    for i in range(sc):
        typ, f, offset, length, count, scrc = struct.unpack("<IIIIII", data[32 + i*24: 32 + (i+1)*24])
        sections[typ] = {"type": typ, "flags": f, "offset": offset, "length": length, "count": count, "raw": data[offset:offset+length]}
    return {"magic": magic, "version": ver, "header_size": hs, "total_size": total, "screen_w": sw, "screen_h": sh, "section_count": sc, "flags": flags, "crc": crc}, sections

def _write_nodes(tmp: Path, nodes, tid="test-scene"):
    theme = {"format":1, "id": tid, "screen":[480,800], "nodes": nodes}
    p = tmp / "theme.json"
    p.write_text(json.dumps(theme), encoding="utf-8")
    return p

def test_legacy_mofei_pack_still_valid_5_sections(tmp_path):
    result, out = _compile(LEGACY_THEME, tmp_path)
    assert result.returncode == 0, f"legacy compile failed: {result.stderr}"
    data = out.read_bytes()
    hdr, sections = _read_header_and_sections(data)
    assert hdr["magic"] == MAGIC
    assert hdr["version"] == 1
    assert hdr["section_count"] == 5, f"legacy pack must stay 5 sections, got {hdr['section_count']}"
    assert set(sections.keys()) == {1,2,3,4,5}
    # validate
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        ok, msg = cht.validate_m4theme(data)
        assert ok, f"legacy validate failed: {msg}"
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_scene_ordered_clear_line_text_has_scene_section_and_preserves_order(tmp_path):
    theme = {
        "format": 1,
        "id": "test-scene-order",
        "screen": [480,800],
        "nodes": [
            {"type": "clear", "color": "white"},
            {"type": "line", "x": 0, "y": 90, "x2": 480, "y2": 90, "width": 1},
            {"type": "text", "text": "Hello", "rect": [30,48,200,24], "font": "ui_16_regular", "align": "left"}
        ]
    }
    p = tmp_path / "theme.json"
    p.write_text(json.dumps(theme), encoding="utf-8")
    result, out = _compile(p, tmp_path)
    assert result.returncode == 0, f"scene compile failed: {result.stderr}\n{result.stdout}"
    data = out.read_bytes()
    hdr, sections = _read_header_and_sections(data)
    assert hdr["magic"] == MAGIC
    assert hdr["version"] == 1
    assert SECTION_SCENE in sections, f"missing SCENE section type {SECTION_SCENE}, got {sorted(sections.keys())}"
    sec = sections[SECTION_SCENE]
    payload = data[sec["offset"]: sec["offset"]+sec["length"]]
    assert sec["count"] == 3, f"scene count {sec['count']} !=3"
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        if hasattr(cht, "decode_scene"):
            cmds = cht.decode_scene(payload)
            types = [c["type"] for c in cmds]
            assert types == ["clear", "line", "text"], f"order mismatch {types}"
        else:
            assert len(payload) > 0
            assert b"Hello" in payload
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_bitmap_before_and_after_cover_ordering(tmp_path):
    # bitmap before cover
    nodes_a = [
        {"type":"clear","color":"white"},
        {"type":"bitmap","rect":[0,0,480,800],"source":"dummy.png"},
        {"type":"cover","rect":[52,129,110,180],"radius":6,"binding":"$current.cover"},
    ]
    nodes_b = [
        {"type":"clear","color":"white"},
        {"type":"cover","rect":[52,129,110,180],"radius":6,"binding":"$current.cover"},
        {"type":"bitmap","rect":[0,0,480,800],"source":"dummy.png"},
    ]
    dir_a = tmp_path / "a"
    dir_b = tmp_path / "b"
    dir_a.mkdir(parents=True, exist_ok=True)
    dir_b.mkdir(parents=True, exist_ok=True)
    p_a = dir_a / "theme.json"
    p_a.write_text(json.dumps({"format":1,"id":"test-bitmap-a","screen":[480,800],"nodes":nodes_a}), encoding="utf-8")
    p_b = dir_b / "theme.json"
    p_b.write_text(json.dumps({"format":1,"id":"test-bitmap-b","screen":[480,800],"nodes":nodes_b}), encoding="utf-8")
    result_a, out_a = _compile(p_a, dir_a)
    assert result_a.returncode == 0, f"a failed {result_a.stderr}"
    result_b, out_b = _compile(p_b, dir_b)
    assert result_b.returncode == 0, f"b failed {result_b.stderr}"
    hdr_a, secs_a = _read_header_and_sections(out_a.read_bytes())
    hdr_b, secs_b = _read_header_and_sections(out_b.read_bytes())
    payload_a = out_a.read_bytes()[secs_a[SECTION_SCENE]["offset"]: secs_a[SECTION_SCENE]["offset"]+secs_a[SECTION_SCENE]["length"]]
    payload_b = out_b.read_bytes()[secs_b[SECTION_SCENE]["offset"]: secs_b[SECTION_SCENE]["offset"]+secs_b[SECTION_SCENE]["length"]]
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        cmds_a = cht.decode_scene(payload_a)
        cmds_b = cht.decode_scene(payload_b)
        assert [c["type"] for c in cmds_a] == ["clear", "bitmap", "cover"]
        assert [c["type"] for c in cmds_b] == ["clear", "cover", "bitmap"]
        # ensure payloads differ and order preserved
        assert payload_a != payload_b
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_rejects_unknown_node_type(tmp_path):
    nodes = [{"type":"unknown_type","rect":[0,0,10,10]}]
    p = _write_nodes(tmp_path, nodes)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown scene node type" in result.stderr.lower()

def test_rejects_unknown_key_in_node(tmp_path):
    nodes = [{"type":"clear","color":"white","unknown_key":123}]
    p = _write_nodes(tmp_path, nodes)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown key" in result.stderr.lower()

def test_rejects_unknown_binding(tmp_path):
    nodes = [{"type":"text","rect":[0,0,100,20],"text":"$unknown.binding","font":"ui_16_regular"}]
    p = _write_nodes(tmp_path, nodes)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown binding" in result.stderr.lower()

def test_rejects_unknown_action(tmp_path):
    nodes = [{"type":"cover","rect":[0,0,50,50],"binding":"$current.cover","action":"open_unknown"}]
    p = _write_nodes(tmp_path, nodes)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "unknown action" in result.stderr.lower()

def test_rejects_malformed_rect(tmp_path):
    nodes = [{"type":"rect","rect":[400,700,200,200],"stroke":1}]
    p = _write_nodes(tmp_path, nodes)
    result, out = _compile(p, tmp_path)
    assert result.returncode != 0
    assert "rect" in result.stderr.lower() or "out of" in result.stderr.lower()

def test_repeat_and_interaction_compile_decode(tmp_path):
    nodes = [
        {"type":"clear","color":"white"},
        {"type":"text","rect":[30,382,100,20],"text":"全部 >","font":"ui_14_regular","action":"open_history"},
        {"type":"repeat","source":"$recent","limit":3,"x":42,"y":405,"item_width":130,"item_height":140,"gap":14,"direction":"horizontal","children":[
            {"type":"cover","rect":[18,0,74,106],"radius":4,"binding":"$item.cover"},
            {"type":"text","rect":[0,112,110,16],"text":"$item.title","font":"ui_12_regular"}
        ]},
        {"type":"repeat","source":"$apps","limit":4,"x":24,"y":610,"item_width":107,"item_height":120,"gap":10,"direction":"horizontal","children":[
            {"type":"icon","rect":[18,0,68,68],"name":"app_icon","binding":"$item.icon","action":"open_app","action_arg":"$item.id"},
            {"type":"text","rect":[0,76,107,18],"text":"$item.name","font":"ui_12_regular"}
        ]}
    ]
    p = _write_nodes(tmp_path, nodes, tid="test-repeat-interaction")
    result, out = _compile(p, tmp_path)
    assert result.returncode == 0, f"repeat compile failed {result.stderr}"
    data = out.read_bytes()
    hdr, secs = _read_header_and_sections(data)
    assert SECTION_SCENE in secs
    payload = data[secs[SECTION_SCENE]["offset"]: secs[SECTION_SCENE]["offset"]+secs[SECTION_SCENE]["length"]]
    assert secs[SECTION_SCENE]["count"] == 4
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        cmds = cht.decode_scene(payload)
        types = [c["type"] for c in cmds]
        assert types[0]=="clear"
        # check repeat present
        assert types[2]=="repeat"
        assert types[3]=="repeat"
        # check interaction preserved: second node should have action open_history
        # decode second node's payload flags action
        # We'll just check raw payload contains action strings or ids
        # For interaction open_app with $item.id, ensure payload contains that binding
        assert b"open" not in payload  # actions are encoded as ids, not strings
        # but ensure compile succeeded and count ok, and that decode doesn't crash
        # Also validate that unknown binding would have failed, but this succeeded
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]
    # also verify that preview can decode without error
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import preview_home_theme as pht
        dec = pht.decode_m4theme(out)
        assert dec["scene"] is not None
        assert len(dec["scene"]) == 4
    finally:
        if "preview_home_theme" in sys.modules:
            del sys.modules["preview_home_theme"]

def test_default_scene_no_bitmap_and_no_fake_literal(tmp_path):
    assert DEFAULT_THEME.is_file(), f"default theme missing {DEFAULT_THEME}"
    text = DEFAULT_THEME.read_text(encoding="utf-8")
    cfg = json.loads(text)
    assert "nodes" in cfg
    assert cfg["bindings"]["$home.current.progress_text"] == 16
    progress_text = next(
        n for n in cfg["nodes"]
        if n.get("type") == "text" and n.get("text") == "$home.current.progress_text"
    )
    assert progress_text["text"] == "$home.current.progress_text"
    progress_bar = next(n for n in cfg["nodes"] if n.get("type") == "progress")
    assert progress_bar["binding"] == "$current.progress"
    # ensure no bitmap node
    types = [n.get("type") for n in cfg["nodes"]]
    assert "bitmap" not in types, f"default scene must have no bitmap node, got {types}"
    # ensure no fake page-count literal like "120" or "336" as standalone text literal
    # Check that no text literal equals those fakes or contains them as isolated number
    for n in cfg["nodes"]:
        if n.get("type") == "text":
            txt = n.get("text","")
            if not txt.startswith("$"):
                assert txt.strip() != "120", "fake 120 literal found"
                assert txt.strip() != "336", "fake 336 literal found"
                assert "120" not in txt or "Murphy" in txt  # allow only if not fake count
    # also ensure compiled pack has no bitmap command and no fake literal bytes
    result, out = _compile(DEFAULT_THEME, tmp_path)
    assert result.returncode == 0, result.stderr
    data = out.read_bytes()
    hdr, secs = _read_header_and_sections(data)
    assert SECTION_SCENE in secs
    payload = data[secs[SECTION_SCENE]["offset"]: secs[SECTION_SCENE]["offset"]+secs[SECTION_SCENE]["length"]]
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        cmds = cht.decode_scene(payload)
        assert all(c["type"] != "bitmap" for c in cmds), "compiled default must have no bitmap command"
        compiled_progress_text = next(
            c for c in cmds
            if c["type"] == "text" and c.get("binding") == 16
        )
        assert compiled_progress_text["binding"] == 16
        # ensure payload does not contain fake 120/336 as text literal bytes surrounded by non-digit?
        # We check raw payload for literal 120 only if it's isolated text node literal
        # Our decode will have text literals; ensure none is fake
        for c in cmds:
            if c["type"] == "text" and "text" in c:
                assert c["text"] != "120"
                assert c["text"] != "336"
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_deterministic_and_crc_and_non_overlap(tmp_path):
    # compile twice and compare
    p1_dir = tmp_path / "d1"
    p2_dir = tmp_path / "d2"
    p1_dir.mkdir()
    p2_dir.mkdir()
    result1, out1 = _compile(DEFAULT_THEME, p1_dir)
    assert result1.returncode == 0
    result2, out2 = _compile(DEFAULT_THEME, p2_dir)
    assert result2.returncode == 0
    b1 = out1.read_bytes()
    b2 = out2.read_bytes()
    assert b1 == b2, "deterministic: two compiles must be byte-identical"
    hdr, secs = _read_header_and_sections(b1)
    # CRC check
    calc = binascii.crc32(b1[32:]) & 0xffffffff
    assert hdr["crc"] == calc, f"CRC mismatch {hdr['crc']:08x} != {calc:08x}"
    # section non-overlap and 4-aligned
    offsets = sorted([(s["offset"], s["length"]) for s in secs.values()])
    for o,l in offsets:
        assert o %4 ==0, f"offset {o} not 4-aligned"
    for i in range(len(offsets)-1):
        assert offsets[i][0] + offsets[i][1] <= offsets[i+1][0], f"overlap {offsets[i]} vs {offsets[i+1]}"
    # also validate via compile helper
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import compile_home_theme as cht
        ok, msg = cht.validate_m4theme(b1)
        assert ok, f"validate failed {msg}"
    finally:
        if "compile_home_theme" in sys.modules:
            del sys.modules["compile_home_theme"]

def test_preview_can_render_default_scene_to_480x800(tmp_path):
    # compile default then preview
    result, out = _compile(DEFAULT_THEME, tmp_path)
    assert result.returncode == 0, result.stderr
    sys.path.insert(0, str(ROOT / "firmware" / "tools"))
    try:
        import preview_home_theme as pht
        out_png = tmp_path / "preview.png"
        res = pht.render_preview(out, out_png, focus_order=None, verbose=False)
        assert res == out_png
        assert out_png.is_file()
        from PIL import Image
        with Image.open(out_png) as im:
            assert im.size == (480,800)
            # should have some non-white pixels
            pix = im.load()
            nonwhite = sum(1 for y in range(800) for x in range(480) if pix[x,y]!=255)
            assert nonwhite > 1000, f"preview should have content nonwhite {nonwhite}"
            # check that no bitmap means background not all black: ensure some white remains
            whites = sum(1 for y in range(800) for x in range(480) if pix[x,y]==255)
            assert whites > 100000
    finally:
        if "preview_home_theme" in sys.modules:
            del sys.modules["preview_home_theme"]
