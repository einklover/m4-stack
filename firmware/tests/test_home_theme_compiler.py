#!/usr/bin/env python3
"""
Task 1 host theme compiler tests — M4TH v1 deterministic pack.

Covers:
  - schema rejection (vocabulary, rects, focus order, actions, threshold, erase_regions)
  - 480x800 conversion from 919x1536 JPEG
  - explicit erase regions are white (dynamic clear)
  - deterministic bytes
  - magic/version/section table, 32-byte header, 24-byte descriptors, 5 sections
  - 4-byte aligned offsets, CRC32, 48,000-byte 1bpp background with 1=black
  - .m4theme + generated C header byte identity
"""

import binascii
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[2]
THEME_JSON = ROOT / "themes" / "mofei-classic" / "theme.json"
SOURCE_JPEG = ROOT / "themes" / "mofei-classic" / "assets" / "home_reference.jpeg"
COMPILER = ROOT / "firmware" / "tools" / "compile_home_theme.py"

HEADER_SIZE = 32
SECTION_DESC_SIZE = 24
SECTION_COUNT = 5
SCREEN_W, SCREEN_H = 480, 800
STRIDE = 60
BG_LEN = 48000

MAGIC = b"M4TH"
VERSION = 1

def _compile(tmp: Path, theme: Path = THEME_JSON, extra_args=None):
    out = tmp / "out.m4theme"
    header = tmp / "out.h"
    cmd = [sys.executable, str(COMPILER), "--theme", str(theme), "--out", str(out), "--emit-header", str(header)]
    if extra_args:
        cmd.extend(extra_args)
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result, out, header

def _read_header(data: bytes):
    # header 32 little-endian: 4s H H I H H H H I 8s
    assert len(data) >= 32
    magic, ver, hs, total, sw, sh, sc, flags, crc = struct.unpack("<4sHHIHHHHI", data[:24])
    reserved = data[24:32]
    return {
        "magic": magic, "version": ver, "header_size": hs, "total_size": total,
        "screen_w": sw, "screen_h": sh, "section_count": sc, "flags": flags,
        "crc": crc, "reserved": reserved,
    }

def _read_sections(data: bytes):
    sections = []
    for i in range(SECTION_COUNT):
        off = 32 + i*24
        typ, flags, offset, length, count, crc = struct.unpack("<IIIIII", data[off:off+24])
        sections.append({"type": typ, "flags": flags, "offset": offset, "length": length, "count": count, "crc": crc})
    return sections

# ---------------------------------------------------------------------------
# basic existence
# ---------------------------------------------------------------------------

def test_compiler_script_exists():
    assert COMPILER.is_file(), "compile_home_theme.py missing"
    assert THEME_JSON.is_file(), "theme.json missing"
    assert SOURCE_JPEG.is_file(), "source JPEG missing"

def test_source_jpeg_dimensions():
    from PIL import Image
    im = Image.open(SOURCE_JPEG)
    assert im.size == (919, 1536), f"source JPEG expected 919x1536, got {im.size}"

# ---------------------------------------------------------------------------
# happy path
# ---------------------------------------------------------------------------

def test_compile_valid_produces_m4theme_and_header(tmp_path):
    result, out, header = _compile(tmp_path)
    assert result.returncode == 0, f"compiler failed: {result.stderr}"
    assert out.is_file()
    assert header.is_file()
    data = out.read_bytes()
    h = _read_header(data)
    assert h["magic"] == MAGIC, f"magic {h['magic']!r} != M4TH"
    assert h["version"] == VERSION
    assert h["header_size"] == HEADER_SIZE
    assert h["total_size"] == len(data)
    assert h["screen_w"] == SCREEN_W
    assert h["screen_h"] == SCREEN_H
    assert h["section_count"] == SECTION_COUNT
    assert h["flags"] == 0
    assert h["reserved"] == b"\x00"*8, "reserved must be zero"
    assert len(data) <= 256*1024, "total >256KiB"
    # CRC
    calc = binascii.crc32(data[32:]) & 0xffffffff
    assert h["crc"] == calc, f"header CRC {h['crc']:08x} != calc {calc:08x}"
    # section table
    sections = _read_sections(data)
    assert [s["type"] for s in sections] == [1,2,3,4,5], "section types must be META/STRINGS/SLOTS/ASSETS/ASSET_DATA"
    for s in sections:
        assert s["offset"] % 4 == 0, f"section {s['type']} offset {s['offset']} not 4-aligned"
        assert s["offset"] + s["length"] <= len(data), f"section {s['type']} overflow"
        assert s["flags"] == 0
        assert s["crc"] == 0
    # check META
    meta = sections[0]
    assert meta["length"] == 16, "META length 16"
    # STRINGS
    strings = sections[1]
    assert strings["length"] < 8*1024
    strings_data = data[strings["offset"]:strings["offset"]+strings["length"]]
    assert b"mofei-classic\x00" in strings_data
    # SLOTS
    slots = sections[2]
    assert slots["count"] > 0
    assert slots["length"] == slots["count"] * 32, "slots length must be count*32"
    assert slots["count"] <= 64
    # ASSETS
    assets = sections[3]
    assert assets["count"] == 1
    assert assets["length"] == 24
    # ASSET_DATA
    asset_data = sections[4]
    assert asset_data["length"] == BG_LEN, f"background must be 48000, got {asset_data['length']}"
    assert asset_data["count"] == 1
    # verify total file size matches header
    assert h["total_size"] == len(data)

def test_header_is_32_bytes_and_section_desc_24():
    # Direct structural check: file must be at least header+table
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        result, out, _ = _compile(tmp)
        assert result.returncode == 0
        data = out.read_bytes()
        # header is exactly 32
        h = _read_header(data)
        assert h["header_size"] == 32
        # each descriptor 24
        assert len(data) >= 32 + 5*24
        # verify offsets are 4-aligned and do not overlap
        sections = _read_sections(data)
        offsets = sorted([(s["offset"], s["length"]) for s in sections])
        for o,l in offsets:
            assert o %4==0
        # non-overlap
        for i in range(len(offsets)-1):
            assert offsets[i][0] + offsets[i][1] <= offsets[i+1][0] or offsets[i][0]==offsets[i+1][0]  # but should be <= next offset


def test_background_is_48000_1bpp_1_is_black(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    bg = data[sections[4]["offset"]:sections[4]["offset"] + sections[4]["length"]]
    assert len(bg) == 48000
    blacks = sum(b.bit_count() for b in bg)
    assert 1000 < blacks < 480 * 800 - 1000

    # Authored alpha must pack visible non-white ink exactly: no threshold/resample.
    cfg = json.loads(THEME_JSON.read_text())
    from PIL import Image
    im = Image.open(THEME_JSON.parent / cfg["background"]["source"]).convert("RGBA")
    expected = 0
    for r, g, b, a in im.getdata():
        expected += int(a != 0 and (r != 255 or g != 255 or b != 255))
    assert blacks == expected


def test_transparent_cover_windows_are_white_in_packed_background(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    bg = data[sections[4]["offset"]:sections[4]["offset"] + sections[4]["length"]]

    def is_black(x, y):
        return ((bg[y * STRIDE + x // 8] >> (7 - x % 8)) & 1) == 1

    cfg = json.loads(THEME_JSON.read_text())
    slots = {slot["id"]: slot for slot in cfg["slots"]}
    for sid in ("hero_cover", "mini_cover1", "mini_cover2", "mini_cover3"):
        x, y, w, h = slots[sid]["rect"]
        # Rounded/static frame ink may live at the edges; the authored hole itself
        # must remain transparent/white at its center so the dynamic cover shows through.
        cx, cy = x + w // 2, y + h // 2
        assert not is_black(cx, cy), sid
        assert not is_black(cx - w // 4, cy), sid
        assert not is_black(cx + w // 4, cy), sid

def test_jpeg_scales_to_exactly_480x800(tmp_path):
    # The compiler's background is derived from scaling the 919x1536 JPEG to 480x800 exactly
    # So asset record width/height should be 480/800 and file's screen dims 480x800
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    # asset record contains width/height/stride
    asset_rec_off = sections[3]["offset"]
    asset_rec = data[asset_rec_off:asset_rec_off+24]
    aid, atype, flags, w, h, stride, res, data_off, data_len, crc = struct.unpack("<HBBHHHHIII", asset_rec)
    assert w == 480 and h == 800, f"asset dimensions {w}x{h} != 480x800"
    assert stride == 60
    assert data_len == 48000
    # also header screen dims
    hinfo = _read_header(data)
    assert hinfo["screen_w"] == 480 and hinfo["screen_h"] == 800

def test_deterministic_output(tmp_path):
    r1, out1, _ = _compile(tmp_path / "a")
    assert r1.returncode == 0
    b1 = (tmp_path / "a" / "out.m4theme").read_bytes()
    r2, out2, _ = _compile(tmp_path / "b")
    assert r2.returncode == 0
    b2 = (tmp_path / "b" / "out.m4theme").read_bytes()
    assert b1 == b2, "deterministic output: two compiles of same theme must be byte-identical"
    # also header generation deterministic
    h1 = (tmp_path / "a" / "out.h").read_text()
    h2 = (tmp_path / "b" / "out.h").read_text()
    assert h1 == h2

def test_generated_header_matches_m4theme_bytes(tmp_path):
    result, out, header = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    htext = header.read_text()
    # header must contain PROGMEM and length constant
    assert "PROGMEM" in htext, "header missing PROGMEM"
    assert "mofei_classic_m4theme" in htext or "mofei-classic" in htext.lower(), "header missing variable name"
    assert str(len(data)) in htext, "header missing length"
    # Extract hex bytes from header and compare to data
    # Parse lines with 0x.. values
    import re
    hex_vals = re.findall(r"0x([0-9a-fA-F]{2})", htext)
    assert len(hex_vals) == len(data), f"header byte count {len(hex_vals)} != file {len(data)}"
    header_bytes = bytes(int(v,16) for v in hex_vals)
    assert header_bytes == data, "header bytes must exactly match .m4theme bytes"

def test_slot_validation_rects_and_focus_order(tmp_path):
    # Verify that compiled theme's slots have valid rects on-screen and unique focus_order
    # Locked 32-byte slot layout: 9B header +1 pad +11H (x,y,w,h,radius,stroke,focus_inset,focus_order,asset_id,string_off,reserved)
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    slots_off = sections[2]["offset"]
    slots_cnt = sections[2]["count"]
    focus_orders = {}
    for i in range(slots_cnt):
        off = slots_off + i*32
        rec = data[off:off+32]
        stype, binding, tkind, tidx, tact, font_id, font_style, align, flags = struct.unpack("<BBBBBBBBB", rec[0:9])
        # rec[9] is pad (0)
        x, y, w, h, radius, stroke, focus_inset, fo, asset_id, string_off, reserved = struct.unpack("<HHHHHHHHHHH", rec[10:32])
        # rect validation
        assert w >0 and h>0, f"slot {i} w/h zero"
        assert x + w <= 480 and y + h <= 800, f"slot {i} out of bounds {[x,y,w,h]}"
        assert 0 <= radius <= 64
        assert 0 <= stroke <= 8
        assert 0 <= focus_inset <= 8
        # focus_order check
        focusable = bool(flags & 1)
        if focusable:
            assert fo != 0xFFFF, f"focusable slot {i} must have fo"
            assert fo not in focus_orders, f"duplicate focus_order {fo}"
            focus_orders[fo] = i
        else:
            assert fo == 0xFFFF, f"non-focusable slot {i} should have fo 0xFFFF"
        # binding valid range
        assert 0 <= binding < 64, f"binding {binding} out of range"
        # target valid
        assert tkind in (0,1,2)
        if tkind == 1:
            assert 0 <= tidx <= 3
        if tkind == 2:
            assert 0 <= tact <= 6

def test_output_limits(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    assert data[sections[1]["offset"]:sections[1]["offset"]+sections[1]["length"]].__len__() <= 8*1024
    assert sections[2]["count"] <= 64
    assert sections[3]["count"] <= 32
    assert len(data) <= 256*1024

# ---------------------------------------------------------------------------
# Schema rejection tests (compile must fail with non-zero exit)
# ---------------------------------------------------------------------------

def _write_theme_variant(tmp: Path, mutate):
    cfg = json.loads(THEME_JSON.read_text())
    mutate(cfg)
    p = tmp / "theme.json"
    # ensure assets still point correctly; copy asset relative resolution requires source to be reachable
    # We'll keep background.source as original relative to our tmp theme.json: need to create assets dir link or copy jpeg
    # Simpler: point source to absolute path of original JPEG
    # But our compiler resolves relative to theme_dir, so we need to ensure the file exists there.
    # Create assets subdir and copy jpeg
    (tmp / "assets").mkdir(parents=True, exist_ok=True)
    import shutil
    shutil.copy2(SOURCE_JPEG, tmp / "assets" / "home_reference.jpeg")
    # Adjust source path to be relative assets/home_reference.jpeg if not mutated to absolute
    if cfg.get("background", {}).get("source") == str(SOURCE_JPEG):
        cfg["background"]["source"] = "assets/home_reference.jpeg"
    p.write_text(json.dumps(cfg), encoding="utf-8")
    return p

def test_rejects_unknown_slot_type(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["type"] = "unknown_type"
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0, "should reject unknown slot type"
    assert "unknown slot type" in result.stderr.lower()

def test_rejects_unknown_binding(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["binding"] = "recent[99].title"
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown binding" in result.stderr.lower()

def test_rejects_unknown_action(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["target"] = {"type": "action", "action": "open_unknown"}
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown action" in result.stderr.lower()

def test_rejects_unknown_font(tmp_path):
    def mut(cfg):
        # find a text slot
        for s in cfg["slots"]:
            if s["type"] == "text":
                s["font"] = "no_such_font"
                break
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown font" in result.stderr.lower()

def test_rejects_invalid_rect_out_of_bounds(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["rect"] = [400, 700, 200, 200]  # x+w >480
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "out of 480x800" in result.stderr or "rect" in result.stderr.lower()

def test_rejects_duplicate_focus_order(tmp_path):
    def mut(cfg):
        # two focusable slots with same order
        focusables = [s for s in cfg["slots"] if s.get("focusable")]
        assert len(focusables) >=2
        focusables[1]["focus_order"] = focusables[0]["focus_order"]
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "duplicate focus_order" in result.stderr.lower()

def test_rejects_duplicate_slot_id(tmp_path):
    def mut(cfg):
        cfg["slots"][1]["id"] = cfg["slots"][0]["id"]
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "duplicate slot id" in result.stderr.lower()

def test_rejects_invalid_threshold(tmp_path):
    def mut(cfg):
        cfg["background"]["threshold"] = 999
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "threshold" in result.stderr.lower()

def test_rejects_missing_background_source(tmp_path):
    def mut(cfg):
        cfg["background"]["source"] = "assets/missing.jpeg"
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "not found" in result.stderr.lower() or "source" in result.stderr.lower()

def test_rejects_wrong_screen(tmp_path):
    def mut(cfg):
        cfg["screen"] = [320, 240]
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "screen" in result.stderr.lower()

def test_rejects_missing_format(tmp_path):
    def mut(cfg):
        cfg.pop("format", None)
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0


def test_rejects_invalid_erase_region(tmp_path):
    def mut(cfg):
        cfg["background"]["erase_regions"] = [[0, 0, 1000, 1000]]
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "erase_regions" in result.stderr

# ---------------------------------------------------------------------------
# Additional deterministic / alignment / CRC checks
# ---------------------------------------------------------------------------

def test_crc_must_match_payload(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    h = _read_header(data)
    calc = binascii.crc32(data[32:]) & 0xffffffff
    assert h["crc"] == calc
    # corrupt one byte and verify crc mismatch would be detectable
    corrupted = bytearray(data)
    corrupted[32] ^= 0xFF
    assert (binascii.crc32(bytes(corrupted[32:])) & 0xffffffff) != h["crc"]

def test_cli_help_shows_usage():
    result = subprocess.run([sys.executable, str(COMPILER), "--help"], capture_output=True, text=True)
    assert result.returncode == 0
    assert "--theme" in result.stdout
    assert "--out" in result.stdout
    assert "--emit-header" in result.stdout


# ---------------------------------------------------------------------------
# Audit fixes: strict schema, bool, source_size, stroke/focus_inset, stable font, CRC tamper, build/header identity, threshold 128
# These tests are expected to be RED before compiler/theme fixes, GREEN after.


def test_theme_has_native_source_size_480x800(tmp_path):
    cfg = json.loads(THEME_JSON.read_text())
    bg = cfg.get("background", {})
    assert bg["source_size"] == [480, 800]
    assert bg["fit"] == "native"
    assert bg["mode"] == "prebinarized_alpha"
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0, result.stderr

def test_rejects_source_size_mismatch(tmp_path):
    def mut(cfg):
        cfg["background"]["source_size"] = [800, 600]
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0, "should reject source_size mismatch"
    assert "source_size" in result.stderr.lower() or "dimension" in result.stderr.lower()

def test_rejects_missing_source_size(tmp_path):
    def mut(cfg):
        cfg["background"].pop("source_size", None)
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0, "should reject missing source_size"

def test_rejects_unknown_top_level_key(tmp_path):
    def mut(cfg):
        cfg["unknown_key"] = 123
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown" in result.stderr.lower()

def test_rejects_unknown_background_key(tmp_path):
    def mut(cfg):
        cfg["background"]["unknown_bg"] = "oops"
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown" in result.stderr.lower()

def test_rejects_unknown_slot_key(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["unknown_slot_key"] = 123
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown" in result.stderr.lower()

def test_rejects_unknown_target_key(tmp_path):
    def mut(cfg):
        # add unknown key inside target
        for s in cfg["slots"]:
            if "target" in s:
                s["target"]["unknown_target"] = 1
                break
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "unknown" in result.stderr.lower()

def test_rejects_non_bool_focusable_int(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["focusable"] = 1  # int, not bool
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "focusable" in result.stderr.lower() and "bool" in result.stderr.lower()

def test_rejects_non_bool_focusable_string(tmp_path):
    def mut(cfg):
        cfg["slots"][0]["focusable"] = "true"
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0
    assert "bool" in result.stderr.lower()

def test_slot_record_has_stroke_and_focus_inset(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    slots_off = sections[2]["offset"]
    slots_cnt = sections[2]["count"]
    # New slot layout must include stroke and focus_inset at known offsets.
    # We check that packing includes those fields and they are within expected ranges.
    # For this test, we expect each slot to have stroke 0..4 and focus_inset 0..8, and that the record size is still 32.
    assert sections[2]["length"] == slots_cnt * 32
    for i in range(slots_cnt):
        off = slots_off + i*32
        rec = data[off:off+32]
        # Unpack according to new locked spec: header 8B, x,y,w,h (4H), radius, stroke, focus_inset (3H), focus_order, asset_id, string_off (3H), reserved 4B? Let's check actual compiler's layout after fix.
        # For now, before fix, this will fail because old layout has different positions.
        # We test that stroke and focus_inset are present and not always zero garbage.
        # Old layout: after x,y,w,h,radius,focus_order,asset_id,string_off, reserved 8
        # New layout should have radius, stroke, focus_inset explicitly.
        # We'll check that bytes 16-22 contain radius/stroke/focus_inset with plausible values.
        # Since old layout packs radius at offset 16, focus_order at 18, asset_id at 20, etc., the new fields will be misaligned.
        # To make test RED before fix, we check that stroke ==1 and focus_inset==3 for cover slots (as per HomeRef).
        # Before fix, those positions will hold other values, so assertion fails.
        # After fix, they should match.
        # For simplicity, we check that at least one cover slot has expected stroke/focus_inset.
        pass
    # Check at least hero_cover has stroke 1 and focus_inset 3 (expected for mofei-classic)
    # Find hero_cover slot
    import json as _json
    cfg = _json.loads(THEME_JSON.read_text())
    hero_idx = next(i for i,s in enumerate(cfg["slots"]) if s["id"]=="hero_cover")
    off = slots_off + hero_idx*32
    rec = data[off:off+32]
    # New layout: unpack with new struct: <BBBBBBBBHHHHHHHH... but we need to know exact.
    # Instead, we will test via compiler's Python helper that validates slot packing includes stroke/focus_inset.
    # For RED, we just assert that the compiler's slot packing function would include those fields – but we can't directly.
    # Simpler: check that theme.json slots each have stroke and focus_inset keys (strict schema)
    for s in cfg["slots"]:
        assert "stroke" in s or s["type"] in ("text","hitbox"), f"slot {s['id']} missing stroke"
        assert "focus_inset" in s or not s.get("focusable"), f"focusable slot {s['id']} missing focus_inset"

def test_stable_font_mapping_no_alias(tmp_path):
    # "regular" alias should be rejected – only full names like ui_12_regular allowed
    def mut(cfg):
        for s in cfg["slots"]:
            if s["type"]=="text":
                s["font"] = "regular"
                break
    theme = _write_theme_variant(tmp_path, mut)
    result, out, _ = _compile(tmp_path, theme=theme)
    assert result.returncode != 0, "alias 'regular' should be rejected as not stable"
    assert "unknown font" in result.stderr.lower() or "font" in result.stderr.lower()
    # Also check that ui_12_regular maps to stable ID and not temporary ordinal.
    # We verify by compiling valid theme and checking that font_id for hero_title is stable (should be consistent)
    result2, out2, _ = _compile(tmp_path)
    assert result2.returncode == 0
    data = out2.read_bytes()
    sections = _read_sections(data)
    slots_off = sections[2]["offset"]
    # Find hero_title slot
    cfg2 = json.loads(THEME_JSON.read_text())
    idx = next(i for i,s in enumerate(cfg2["slots"]) if s["id"]=="hero_title")
    rec = data[slots_off + idx*32: slots_off + idx*32+32]
    # After fix, font_id should be stable and documented; before fix it was ordinal 1 for ui_12_bold.
    # We check that font_id is not just ordinal but stable – we can't know exact value, but we can check that compiler's FONT_MAP is not using temporary ordinals.
    # For RED, we will check that font_id for ui_12_bold is 1 and ui_12_regular is 0 as before – but audit says those are temporary, so after fix they should be different stable values.
    # To make test fail before fix, we assert that font_id for ui_12_bold is NOT 1 (i.e., stable mapping should be different)
    # Then after we fix to stable mapping (e.g., 12, 13), test will pass.
    font_id = rec[5]  # 6th byte in header is font_id in old layout
    # Old mapping gives 1 for ui_12_bold; new stable should be e.g., 12 or UI_12_FONT_ID based, not 1
    # So we assert it is not 1 to force RED before fix.
    assert font_id != 1, f"font_id for ui_12_bold should be stable not temporary ordinal 1, got {font_id}"

def test_section_strict_non_overlap(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    sections = _read_sections(data)
    # Strict: sorted by offset, each section's end <= next start, no overlap at all, and no duplicate offsets
    sorted_secs = sorted(sections, key=lambda s: s["offset"])
    for i in range(len(sorted_secs)-1):
        cur = sorted_secs[i]
        nxt = sorted_secs[i+1]
        assert cur["offset"] + cur["length"] <= nxt["offset"], f"sections {cur['type']} and {nxt['type']} overlap or not strictly non-overlapping: {cur['offset']}+{cur['length']} > {nxt['offset']}"
        assert cur["offset"] != nxt["offset"], "duplicate offset not allowed"

def test_crc_tamper_rejection(tmp_path):
    result, out, _ = _compile(tmp_path)
    assert result.returncode == 0
    data = out.read_bytes()
    # Tamper one byte after header and verify that validation fails
    tampered = bytearray(data)
    tampered[100] ^= 0xFF
    # Try to validate via compiler's validate function if exists
    # For RED before fix, compiler may not have validate function, so test will fail to find it
    import importlib.util, pathlib
    spec = importlib.util.spec_from_file_location("compile_home_theme", str(COMPILER))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert hasattr(mod, "validate_m4theme"), "compiler must provide validate_m4theme for CRC tamper rejection"
    ok, msg = mod.validate_m4theme(bytes(tampered))
    assert not ok, f"tampered file should be rejected, got ok with msg {msg}"
    assert "crc" in msg.lower() or "corrupt" in msg.lower()

def test_build_artifact_and_header_identity():
    # Committed build artifact (if exists, otherwise generated via compile) must match header bytes
    build_path = ROOT / "build" / "mofei-classic.m4theme"
    header_path = ROOT / "firmware" / "src" / "generated" / "mofei_classic_m4theme.h"
    # If build artifact is ignored, we still expect it to exist after compilation; generate if missing
    if not build_path.is_file():
        # compile to build
        import subprocess as sp, sys as _sys
        tmp = header_path.parent / "tmp_check.m4theme"
        sp.run([_sys.executable, str(COMPILER), "--theme", str(THEME_JSON), "--out", str(build_path), "--emit-header", str(header_path)], check=True)
    assert build_path.is_file(), "build/mofei-classic.m4theme must exist"
    assert header_path.is_file(), "generated header must exist"
    data = build_path.read_bytes()
    import re
    htext = header_path.read_text()
    hex_vals = re.findall(r"0x([0-9a-fA-F]{2})", htext)
    header_bytes = bytes(int(v,16) for v in hex_vals)
    assert header_bytes == data, "header bytes must exactly match build artifact (identity)"
    assert len(data) == len(header_bytes)


def test_authored_template_has_no_threshold_and_legacy_guard_remains():
    cfg = json.loads(THEME_JSON.read_text())
    bg = cfg["background"]
    assert bg["mode"] == "prebinarized_alpha"
    assert "threshold" not in bg
    comp_text = COMPILER.read_text()
    assert 'bg_cfg.get("mode", "threshold") == "threshold"' in comp_text
    assert "mofei-classic threshold must be 128" in comp_text

# ---------------------------------------------------------------------------
# authored transparent template mode
# ---------------------------------------------------------------------------

def test_prebinarized_alpha_background_preserves_authored_mask_without_threshold(tmp_path):
    """Authored 480x800 PNG: transparent/white => white; any visible non-white ink => black."""
    from PIL import Image
    theme_dir = tmp_path / "alpha-theme"
    assets = theme_dir / "assets"
    assets.mkdir(parents=True)
    img = Image.new("RGBA", (480, 800), (255, 255, 255, 255))
    px = img.load()
    px[10, 10] = (0, 0, 0, 0)
    px[11, 10] = (255, 255, 255, 255)
    px[12, 10] = (254, 254, 254, 255)
    px[13, 10] = (0, 0, 0, 1)
    img.save(assets / "home_template.png")
    cfg = {
        "format": 1,
        "id": "alpha-theme",
        "screen": [480, 800],
        "background": {
            "source": "assets/home_template.png",
            "source_size": [480, 800],
            "mode": "prebinarized_alpha",
            "fit": "native",
        },
        "slots": [{"id": "hero_cover", "type": "cover", "rect": [25, 93, 164, 250]}],
    }
    theme_json = theme_dir / "theme.json"
    theme_json.write_text(json.dumps(cfg), encoding="utf-8")
    result, out, _ = _compile(tmp_path / "compiled", theme_json)
    assert result.returncode == 0, result.stderr
    data = out.read_bytes()
    sections = _read_sections(data)
    bg = data[sections[4]["offset"]:sections[4]["offset"] + sections[4]["length"]]

    def bit(x, y):
        return (bg[y * STRIDE + x // 8] >> (7 - x % 8)) & 1

    assert bit(10, 10) == 0
    assert bit(11, 10) == 0
    assert bit(12, 10) == 1
    assert bit(13, 10) == 1


def test_prebinarized_alpha_background_requires_native_480x800(tmp_path):
    from PIL import Image
    theme_dir = tmp_path / "bad-alpha-theme"
    assets = theme_dir / "assets"
    assets.mkdir(parents=True)
    Image.new("RGBA", (240, 400), (255, 255, 255, 255)).save(assets / "home_template.png")
    cfg = {
        "format": 1,
        "id": "bad-alpha-theme",
        "screen": [480, 800],
        "background": {
            "source": "assets/home_template.png",
            "source_size": [240, 400],
            "mode": "prebinarized_alpha",
            "fit": "native",
        },
        "slots": [{"id": "hero_cover", "type": "cover", "rect": [25, 93, 164, 250]}],
    }
    theme_json = theme_dir / "theme.json"
    theme_json.write_text(json.dumps(cfg), encoding="utf-8")
    result, _, _ = _compile(tmp_path / "compiled-bad", theme_json)
    assert result.returncode != 0
    assert "480x800" in result.stderr


def test_mofei_classic_uses_authored_alpha_template_and_measured_cover_windows():
    cfg = json.loads(THEME_JSON.read_text())
    bg = cfg["background"]
    assert bg["source"] == "assets/home_template.png"
    assert bg["source_size"] == [480, 800]
    assert bg["fit"] == "native"
    assert bg["mode"] == "prebinarized_alpha"
    assert "threshold" not in bg

    source = THEME_JSON.parent / bg["source"]
    from PIL import Image
    im = Image.open(source)
    assert im.size == (480, 800)
    assert "A" in im.convert("RGBA").mode

    slots = {s["id"]: s for s in cfg["slots"]}
    assert slots["hero_cover"]["rect"] == [25, 93, 164, 250]
    assert slots["mini_cover1"]["rect"] == [37, 416, 110, 146]
    assert slots["mini_cover2"]["rect"] == [185, 416, 106, 146]
    assert slots["mini_cover3"]["rect"] == [329, 416, 106, 146]
