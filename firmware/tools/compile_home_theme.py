#!/usr/bin/env python3
"""
M4 Home Theme compiler — M4TH v1.

Strict authoring JSON -> deterministic .m4theme pack.

Spec: docs/superpowers/specs/2026-08-29-home-theme-package.md

Threshold note for mofei-classic: uses 128 (not spec example 170).
Spec illustrative threshold 170 is for example only; mofei-classic threshold
is fixed at 128 to match reference contrast and ensure deterministic
1bpp conversion. Compiler validates threshold is int 0..255 and theme's
threshold must be 128 for mofei-classic; other themes may choose different
thresholds but must be explicit. This note eliminates 170 ambiguity.

Binary layout (little-endian, deterministic, 4-byte aligned):

Header 32 bytes:
  0: magic 4s b"M4TH"
  4: version u16 =1
  6: header_size u16 =32
  8: total_size u32 (file length)
 12: screen_w u16 =480
 14: screen_h u16 =800
 16: section_count u16 =5
 18: flags u16 =0
 20: payload_crc32 u32 (crc32 of bytes after header, i.e. file[32:])
 24: reserved 8s zero

Section descriptor 24 bytes (little-endian):
  0: type u32
  4: flags u32
  8: offset u32 (file offset, 4-aligned)
 12: length u32 (payload byte length, not including alignment pad)
 16: count u32 (record count, 0 if not applicable)
 20: crc32 u32 (0 as covered by global CRC)

Section types (v1):
  1 META
  2 STRINGS
  3 SLOTS
  4 ASSETS
  5 ASSET_DATA

Section payloads (in this order):
  META: 16 bytes
    0: version u16 =1
    2: header_size u16 =32
    4: screen_w u16 =480
    6: screen_h u16 =800
    8: flags u32 =0
   12: reserved u32 =0
  (total 16)

  STRINGS: variable
    Theme id as UTF-8 + NUL terminator, length = len(id)+1.
    Up to 8 KiB. Single string for v1.

  SLOTS: array of fixed 32-byte records (locked spec, includes stroke/focus_inset)
    Layout (little-endian, 32 bytes):
      0: slot_type u8
      1: binding u8
      2: target_kind u8
      3: target_index u8
      4: target_action u8
      5: font_id u8 (stable mapping, see FONT_MAP)
      6: font_style u8 (0=regular, 1=bold, 0xFF=none)
      7: align u8 (0=left,1=center,2=right)
      8: flags u8 (bit0 focusable)
      9: reserved u8 (0)
     10: x u16
     12: y u16
     14: w u16
     16: h u16
     18: radius u16
     20: stroke u16
     22: focus_inset u16
     24: focus_order u16 (0xFFFF = none)
     26: asset_id u16 (0xFFFF = none)
     28: string_off u16 (0xFFFF = none)
     30: reserved u16 (0)
    Total 32. radius/stroke/focus_inset are per spec 6.3.
    font_id is stable enum, not temporary ordinal; style separated.

  ASSETS: array of 24-byte asset records
    One entry for the full-screen background.

  ASSET_DATA: concatenated asset blobs
    Background 1bpp raster 480*800, stride 60, 48000 bytes, 1=black MSB first.

All offsets 4-byte aligned, deterministic, no timestamps.
"""

from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

MAGIC = b"M4TH"
VERSION = 1
HEADER_SIZE = 32
SCREEN_W = 480
SCREEN_H = 800
SECTION_COUNT = 5  # legacy fixed; new packs may have 6+ with optional SCENE
MAX_TOTAL = 256 * 1024
MAX_SLOTS = 64
MAX_ASSETS = 32
MAX_STRINGS = 8 * 1024
MAX_SCENE_NODES = 128
MAX_SCENE_DEPTH = 3
MAX_REPEAT_LIMIT = 8
MAX_REPEAT_CHILDREN = 16

SECTION_META = 1
SECTION_STRINGS = 2
SECTION_SLOTS = 3
SECTION_ASSETS = 4
SECTION_ASSET_DATA = 5
SECTION_SCENE = 6
SECTION_INTERACTIONS = 7

SLOT_SIZE = 32
ASSET_RECORD_SIZE = 24
META_SIZE = 16
SCENE_VERSION = 1

# Scene node types (ordered draw, JSON nodes order = exact draw order)
SCENE_NODE_CLEAR = 0
SCENE_NODE_BITMAP = 1
SCENE_NODE_LINE = 2
SCENE_NODE_RECT = 3
SCENE_NODE_ROUND_RECT = 4
SCENE_NODE_TEXT = 5
SCENE_NODE_COVER = 6  # alias image/cover
SCENE_NODE_PROGRESS = 7
SCENE_NODE_ICON = 8
SCENE_NODE_BATTERY = 9
SCENE_NODE_GROUP = 10
SCENE_NODE_REPEAT = 11

SCENE_NODE_TYPE_MAP = {
    "clear": SCENE_NODE_CLEAR,
    "bitmap": SCENE_NODE_BITMAP,
    "line": SCENE_NODE_LINE,
    "rect": SCENE_NODE_RECT,
    "round_rect": SCENE_NODE_ROUND_RECT,
    "round-rect": SCENE_NODE_ROUND_RECT,
    "text": SCENE_NODE_TEXT,
    "cover": SCENE_NODE_COVER,
    "image": SCENE_NODE_COVER,
    "progress": SCENE_NODE_PROGRESS,
    "icon": SCENE_NODE_ICON,
    "battery": SCENE_NODE_BATTERY,
    "group": SCENE_NODE_GROUP,
    "repeat": SCENE_NODE_REPEAT,
}

# Scene bindings vocabulary (unknown = compile error)
# Covers: $system.battery, $system.wifi.connected, $current.*, $recent, $apps, $item.*
BINDING_SCENE_MAP = {
    "$system.battery": 1,
    "$system.wifi.connected": 2,
    "$current.exists": 10,
    "$current.title": 11,
    "$current.author": 12,
    "$current.source": 13,
    "$current.cover": 14,
    "$current.progress": 15,
    "$recent": 20,
    "$apps": 21,
    "$item.id": 30,
    "$item.name": 31,
    "$item.title": 32,
    "$item.cover": 33,
    "$item.icon": 34,
    "$item.progress": 35,
}

# Scene actions (hit rect/action metadata)
ACTION_SCENE_MAP = {
    "open_current_book": 0,
    "open_history": 1,
    "open_apps": 2,
    "open_app": 3,
}

# finite vocabularies — stable font IDs (not temporary ordinals)
# Only full names allowed; aliases like "regular" are rejected.
FONT_MAP = {
    "ui_10_regular": 10,
    "ui_10_bold": 11,
    "ui_12_regular": 12,
    "ui_12_bold": 13,
    "ui_14_regular": 14,
    "ui_14_bold": 15,
    "ui_16_regular": 16,
    "ui_16_bold": 17,
    "ui_18_regular": 18,
    "ui_18_bold": 19,
    "ui_20_regular": 20,
    "ui_20_bold": 21,
    "ui_22_regular": 22,
    "ui_22_bold": 23,
    "ui_24_regular": 24,
    "ui_24_bold": 25,
    "small_regular": 30,
    "small_bold": 31,
}

# For backwards mapping, we keep that font_style is separate? Actually FONT_MAP already encodes style in name.
# To keep stable, we map full name to small stable ordinal that is NOT just insertion order but explicitly assigned above.
# The style will be derived from name suffix.

FONT_STYLE_MAP = {
    "regular": 0,
    "bold": 1,
}

SLOT_TYPES = {
    "text": 0,
    "image": 1,
    "icon": 1,
    "cover": 2,
    "progress": 3,
    "hitbox": 4,
}

ALIGN_MAP = {
    "left": 0,
    "center": 1,
    "centre": 1,
    "right": 2,
}

# binding enum – finite
BINDING_MAP = {
    "none": 0,
    "": 0,
    "wifi_state": 1,
    "wifi": 1,
    "recent_count": 2,
    "recent_count_label": 2,
    "recentcount": 2,
    "recent[0].title": 3,
    "recent0.title": 3,
    "recent[0].author": 4,
    "recent0.author": 4,
    "recent[0].source": 5,
    "recent0.source": 5,
    "recent[0].cover": 6,
    "recent0.cover": 6,
    "recent[0].progress": 7,
    "recent0.progress": 7,
    "recent[1].title": 8,
    "recent1.title": 8,
    "recent[1].author": 9,
    "recent1.author": 9,
    "recent[1].source": 10,
    "recent1.source": 10,
    "recent[1].cover": 11,
    "recent1.cover": 11,
    "recent[1].progress": 12,
    "recent1.progress": 12,
    "recent[2].title": 13,
    "recent2.title": 13,
    "recent[2].author": 14,
    "recent2.author": 14,
    "recent[2].source": 15,
    "recent2.source": 15,
    "recent[2].cover": 16,
    "recent2.cover": 16,
    "recent[2].progress": 17,
    "recent2.progress": 17,
    "recent[3].title": 18,
    "recent3.title": 18,
    "recent[3].author": 19,
    "recent3.author": 19,
    "recent[3].source": 20,
    "recent3.source": 20,
    "recent[3].cover": 21,
    "recent3.cover": 21,
    "recent[3].progress": 22,
    "recent3.progress": 22,
}

TARGET_KIND_MAP = {
    "none": 0,
    "recent_book": 1,
    "recentbook": 1,
    "action": 2,
}

ACTION_MAP = {
    "open_files": 0,
    "open_file": 0,
    "open_weread": 1,
    "open_fanqie": 2,
    "open_jinjiang": 3,
    "open_history": 4,
    "open_apps": 5,
    "open_settings": 6,
    "files": 0,
    "weread": 1,
    "fanqie": 2,
    "jinjiang": 3,
    "history": 4,
    "apps": 5,
    "settings": 6,
}

ASSET_TYPE_RASTER_1BPP = 0

# Strict schema allowed keys (legacy)
ALLOWED_TOP_KEYS = {"format", "id", "screen", "background", "slots", "nodes", "bindings", "actions"}
# Manifest-based page-specific vocabularies — merged with reserved common
COMMON_SCENE_BINDINGS = {
    "$system.battery": 1,
    "$system.wifi.connected": 2,
    "$item.id": 30,
    "$item.name": 31,
    "$item.title": 32,
    "$item.cover": 33,
    "$item.icon": 34,
    "$item.progress": 35,
}
LEGACY_SCENE_BINDINGS = {
    "$current.exists": 10,
    "$current.title": 11,
    "$current.author": 12,
    "$current.source": 13,
    "$current.cover": 14,
    "$current.progress": 15,
    "$recent": 20,
    "$apps": 21,
}
RESERVED_SCENE_BINDING_NAMES = set(COMMON_SCENE_BINDINGS) | set(LEGACY_SCENE_BINDINGS)
RESERVED_SCENE_BINDING_IDS = set(COMMON_SCENE_BINDINGS.values()) | set(LEGACY_SCENE_BINDINGS.values())
LEGACY_SCENE_ACTIONS = {
    "open_current_book": 0,
    "open_history": 1,
    "open_apps": 2,
    "open_app": 3,
}
RESERVED_SCENE_ACTION_NAMES = set(LEGACY_SCENE_ACTIONS)
RESERVED_SCENE_ACTION_IDS = set(LEGACY_SCENE_ACTIONS.values())
# Active per-compile maps (set in compile_theme, fallback to legacy full map)
_ACTIVE_SCENE_BINDING_MAP: dict | None = None
_ACTIVE_SCENE_ACTION_MAP: dict | None = None
ALLOWED_BG_KEYS = {"source", "source_size", "fit", "mode", "threshold", "erase_regions"}
ALLOWED_SLOT_KEYS = {"id", "type", "binding", "rect", "radius", "stroke", "focus_inset", "focusable", "focus_order", "font", "align", "target", "asset_id"}
ALLOWED_TARGET_KEYS = {"type", "index", "action"}
# Scene schema
ALLOWED_SCENE_TOP_KEYS = {"format", "id", "screen", "nodes", "background", "slots"}
ALLOWED_SCENE_NODE_KEYS = {
    "clear": {"type", "color", "visible_if", "action", "action_arg"},
    "bitmap": {"type", "source", "x", "y", "w", "h", "rect", "visible_if", "action", "action_arg"},
    "line": {"type", "x", "y", "x2", "y2", "width", "color", "visible_if", "action", "action_arg"},
    "rect": {"type", "x", "y", "w", "h", "rect", "stroke", "fill", "color", "visible_if", "action", "action_arg"},
    "round_rect": {"type", "x", "y", "w", "h", "rect", "r", "radius", "stroke", "fill", "color", "visible_if", "action", "action_arg"},
    "text": {"type", "x", "y", "w", "h", "rect", "text", "value", "font", "align", "ellipsis", "visible_if", "action", "action_arg", "color"},
    "cover": {"type", "x", "y", "w", "h", "rect", "radius", "r", "binding", "source", "visible_if", "action", "action_arg"},
    "progress": {"type", "x", "y", "w", "h", "rect", "radius", "r", "binding", "visible_if", "action", "action_arg"},
    "icon": {"type", "x", "y", "w", "h", "rect", "name", "icon", "binding", "visible_if", "action", "action_arg"},
    "battery": {"type", "x", "y", "w", "h", "rect", "binding", "visible_if", "action", "action_arg"},
    "group": {"type", "children", "visible_if", "action", "action_arg"},
    "repeat": {"type", "source", "limit", "x", "y", "item_width", "item_height", "gap", "direction", "layout", "children", "visible_if", "action", "action_arg"},
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _err(msg: str) -> None:
    raise ValueError(msg)

def _align_up(v: int, a: int = 4) -> int:
    return (v + a - 1) // a * a

def _ensure_list(v, name: str):
    if not isinstance(v, list):
        _err(f"{name} must be list")
    return v

def _require_keys(d: dict, keys, ctx: str):
    for k in keys:
        if k not in d:
            _err(f"{ctx} missing required key '{k}'")

def _reject_unknown_keys(d: dict, allowed: set, ctx: str):
    for k in d:
        if k not in allowed:
            _err(f"{ctx} unknown key '{k}' (allowed: {sorted(allowed)})")

def _validate_rect(rect, ctx: str):
    if not isinstance(rect, list) or len(rect) != 4:
        _err(f"{ctx} rect must be [x,y,w,h]")
    x, y, w, h = rect
    for val, n in zip(rect, ("x","y","w","h")):
        if not isinstance(val, int) or isinstance(val, bool):
            _err(f"{ctx} rect {n} must be int")
    if w <= 0 or h <= 0:
        _err(f"{ctx} rect w/h must be >0 (got {w}x{h})")
    if x < 0 or y < 0 or x + w > SCREEN_W or y + h > SCREEN_H:
        _err(f"{ctx} rect {rect} out of 480x800 bounds")
    return (x, y, w, h)

def _map_slot_type(s: str):
    if not isinstance(s, str):
        _err(f"slot type must be string, got {s!r}")
    k = s.lower()
    if k not in SLOT_TYPES:
        _err(f"unknown slot type '{s}' (allowed: {sorted(SLOT_TYPES)})")
    return SLOT_TYPES[k]

def _map_binding(s: str):
    if s is None:
        return 0
    if not isinstance(s, str):
        _err(f"binding must be string, got {s!r}")
    k = s.strip().lower()
    if k == "":
        return 0
    if k not in BINDING_MAP:
        _err(f"unknown binding '{s}' (allowed: {sorted(set(BINDING_MAP))})")
    return BINDING_MAP[k]

def _map_font(s):
    if s is None:
        return (0xFF, 0xFF)
    if not isinstance(s, str):
        _err(f"font must be string, got {s!r}")
    k = s.lower()
    if k not in FONT_MAP:
        _err(f"unknown font '{s}' (allowed: {sorted(FONT_MAP)})")
    # Derive style from name suffix: _regular => 0, _bold =>1
    style = 0xFF
    if k.endswith("_regular"):
        style = 0
    elif k.endswith("_bold"):
        style = 1
    else:
        style = 0
    return (FONT_MAP[k] & 0xFF, style)

def _map_align(s):
    if s is None:
        return 0
    if not isinstance(s, str):
        _err(f"align must be string, got {s!r}")
    k = s.lower()
    if k not in ALIGN_MAP:
        _err(f"unknown align '{s}' (allowed: left/center/right)")
    return ALIGN_MAP[k]

def _map_target(target, ctx: str):
    if target is None:
        return (0, 0, 0)
    if not isinstance(target, dict):
        _err(f"{ctx} target must be object")
    _reject_unknown_keys(target, ALLOWED_TARGET_KEYS, f"{ctx} target")
    t = target.get("type", "none")
    if not isinstance(t, str):
        _err(f"{ctx} target.type must be string")
    k = t.lower()
    if k not in TARGET_KIND_MAP:
        _err(f"{ctx} unknown target type '{t}'")
    kind = TARGET_KIND_MAP[k]
    idx = 0
    act = 0
    if kind == 1:
        if "index" not in target:
            _err(f"{ctx} recent_book target requires index")
        idx = target["index"]
        if not isinstance(idx, int) or isinstance(idx, bool) or not (0 <= idx <= 3):
            _err(f"{ctx} recent_book index must be 0..3, got {idx!r}")
        if "action" in target:
            _err(f"{ctx} recent_book target must not have action")
    elif kind == 2:
        if "action" not in target:
            _err(f"{ctx} action target requires action")
        a = target["action"]
        if not isinstance(a, str):
            _err(f"{ctx} action must be string")
        ak = a.lower()
        if ak not in ACTION_MAP:
            _err(f"{ctx} unknown action '{a}' (allowed: {sorted(set(ACTION_MAP))})")
        act = ACTION_MAP[ak]
        if "index" in target:
            _err(f"{ctx} action target must not have index")
    else:
        if "index" in target or "action" in target:
            _err(f"{ctx} none target must not have index/action")
    return (kind, idx, act)

def _pack_slot(slot: dict) -> bytes:
    """Pack one slot into 32 bytes (locked spec)."""
    _reject_unknown_keys(slot, ALLOWED_SLOT_KEYS, f"slot '{slot.get('id','')}'")
    sid = slot.get("id", "")
    stype = _map_slot_type(slot.get("type", ""))
    rect = slot.get("rect")
    if rect is None:
        _err(f"slot '{sid}' missing rect")
    x, y, w, h = _validate_rect(rect, f"slot '{sid}'")
    binding = _map_binding(slot.get("binding", "none"))
    # font/style handling
    font_id = 0xFF
    font_style = 0xFF
    if "font" in slot:
        fid, fstyle = _map_font(slot["font"])
        font_id = fid
        font_style = fstyle
    elif stype == 0:
        _err(f"slot '{sid}' type text requires font")
    else:
        font_id = 0xFF
        font_style = 0xFF
    align = _map_align(slot.get("align", "left") if stype == 0 else "left")
    radius = int(slot.get("radius", 0))
    if not isinstance(slot.get("radius", 0), int) or isinstance(slot.get("radius", 0), bool) or not (0 <= radius <= 64):
        _err(f"slot '{sid}' radius must be int 0..64")
    stroke = int(slot.get("stroke", 0))
    if "stroke" in slot:
        if not isinstance(slot["stroke"], int) or isinstance(slot["stroke"], bool):
            _err(f"slot '{sid}' stroke must be int")
    if not (0 <= stroke <= 8):
        _err(f"slot '{sid}' stroke must be 0..8")
    focus_inset = int(slot.get("focus_inset", 0))
    if "focus_inset" in slot:
        if not isinstance(slot["focus_inset"], int) or isinstance(slot["focus_inset"], bool):
            _err(f"slot '{sid}' focus_inset must be int")
    if not (0 <= focus_inset <= 8):
        _err(f"slot '{sid}' focus_inset must be 0..8")
    # focusable must be true bool
    if "focusable" in slot:
        if not isinstance(slot["focusable"], bool):
            _err(f"slot '{sid}' focusable must be bool")
        focusable = slot["focusable"]
    else:
        focusable = False
    focus_order = 0xFFFF
    if focusable:
        if "focus_order" not in slot:
            _err(f"slot '{sid}' focusable requires focus_order")
        fo = slot["focus_order"]
        if not isinstance(fo, int) or isinstance(fo, bool) or not (0 <= fo <= 63):
            _err(f"slot '{sid}' focus_order must be int 0..63")
        focus_order = fo
    else:
        if "focus_order" in slot:
            _err(f"slot '{sid}' non-focusable must not have focus_order")
        focus_order = 0xFFFF
    asset_id = 0xFFFF
    if "asset_id" in slot:
        aid = slot["asset_id"]
        if not isinstance(aid, int) or isinstance(aid, bool) or not (0 <= aid <= 31):
            _err(f"slot '{sid}' asset_id must be int 0..31")
        asset_id = aid
    string_off = 0xFFFF
    target_kind, target_idx, target_action = _map_target(slot.get("target"), f"slot '{sid}'")
    flags = 0
    if focusable:
        flags |= 0x01
    # flags strictly 0 or 1 for v1
    # Pack according to locked spec: header 9 bytes + padding + rect + style
    # Use 32-byte layout: B*9 + pad, H*? Need exact.
    # We define: header 9B: type,binding,target_kind,target_idx,target_action,font_id,font_style,align,flags
    # Then 1 byte reserved, then x,y,w,h,radius,stroke,focus_inset,focus_order,asset_id,string_off,reserved
    # Total header 9 +1 pad =10, then 11*2=22 =>32. So 10 +22 =32.
    header = struct.pack("<BBBBBBBBB", stype & 0xFF, binding & 0xFF, target_kind & 0xFF, target_idx & 0xFF, target_action & 0xFF, font_id & 0xFF, font_style & 0xFF, align & 0xFF, flags & 0xFF)
    # pad one byte zero
    header_padded = header + b"\x00"
    assert len(header_padded) == 10
    body = struct.pack("<HHHHHHHHHHH", x & 0xFFFF, y & 0xFFFF, w & 0xFFFF, h & 0xFFFF, radius & 0xFFFF, stroke & 0xFFFF, focus_inset & 0xFFFF, focus_order & 0xFFFF, asset_id & 0xFFFF, string_off & 0xFFFF, 0)
    assert len(body) == 22
    data = header_padded + body
    assert len(data) == 32, f"slot packed len {len(data)} !=32"
    return data

def _pack_asset_record(asset_id: int, width: int, height: int, stride: int, data_offset: int, data_length: int) -> bytes:
    return struct.pack("<HBBHHHHIII", asset_id & 0xFFFF, ASSET_TYPE_RASTER_1BPP & 0xFF, 0, width & 0xFFFF, height & 0xFFFF, stride & 0xFFFF, 0, data_offset & 0xFFFFFFFF, data_length & 0xFFFFFFFF, 0)

# ---------------------------------------------------------------------------
# Scene helpers
# ---------------------------------------------------------------------------

def _get_effective_binding_map() -> dict:
    if _ACTIVE_SCENE_BINDING_MAP is not None:
        return _ACTIVE_SCENE_BINDING_MAP
    return BINDING_SCENE_MAP

def _get_effective_action_map() -> dict:
    if _ACTIVE_SCENE_ACTION_MAP is not None:
        return _ACTIVE_SCENE_ACTION_MAP
    return ACTION_SCENE_MAP

def _map_scene_binding(s: str) -> int:
    if not isinstance(s, str):
        _err(f"binding must be string, got {s!r}")
    m = _get_effective_binding_map()
    if s not in m:
        _err(f"unknown binding '{s}' (allowed: {sorted(m)})")
    return m[s]

def _map_scene_action(s: str) -> int:
    if not isinstance(s, str):
        _err(f"action must be string, got {s!r}")
    k = s.strip()
    m = _get_effective_action_map()
    if k not in m:
        _err(f"unknown action '{s}' (allowed: {sorted(m)})")
    return m[k]

def _parse_rect_from_node(node: dict, ctx: str):
    # supports either rect:[x,y,w,h] or x/y/w/h keys
    if "rect" in node:
        r = node["rect"]
        return _validate_rect(r, ctx)
    # fallback to individual keys
    has_xywh = all(k in node for k in ("x","y","w","h"))
    has_xywh_alt = all(k in node for k in ("x","y","item_width","item_height"))
    if has_xywh:
        x = node["x"]; y = node["y"]; w = node["w"]; h = node["h"]
        if not all(isinstance(v,int) and not isinstance(v,bool) for v in (x,y,w,h)):
            _err(f"{ctx} x/y/w/h must be int")
        _validate_rect([x,y,w,h], ctx)
        return (x,y,w,h)
    return None

def _validate_scene_binding_or_literal(s: str, ctx: str):
    # Text value can be literal or binding starting with $
    if not isinstance(s, str):
        _err(f"{ctx} text value must be string, got {s!r}")
    if s.startswith("$"):
        _map_scene_binding(s)  # validate
        return True  # is binding
    return False

def _encode_scene_string(s: str) -> bytes:
    b = s.encode("utf-8")
    if len(b) > 255:
        _err(f"string too long {len(b)} >255")
    return struct.pack("<H", len(b)) + b

def _encode_scene_node(node: dict, depth: int = 0) -> bytes:
    if depth > MAX_SCENE_DEPTH:
        _err(f"scene max depth {MAX_SCENE_DEPTH} exceeded")
    if not isinstance(node, dict):
        _err(f"scene node must be object, got {node!r}")
    if "type" not in node:
        _err(f"scene node missing 'type'")
    raw_type = node["type"]
    if not isinstance(raw_type, str):
        _err(f"scene node type must be string, got {raw_type!r}")
    tkey = raw_type.lower()
    if tkey not in SCENE_NODE_TYPE_MAP:
        _err(f"unknown scene node type '{raw_type}' (allowed: {sorted(SCENE_NODE_TYPE_MAP)})")
    ntype = SCENE_NODE_TYPE_MAP[tkey]
    canon = None
    for k,v in SCENE_NODE_TYPE_MAP.items():
        if v == ntype:
            # find canonical key for allowed keys check: prefer first mapping
            if k in ALLOWED_SCENE_NODE_KEYS:
                canon = k
                break
    # Determine allowed keys set for this type
    allowed = None
    # map alias to canonical allowed set
    if tkey in ALLOWED_SCENE_NODE_KEYS:
        allowed = ALLOWED_SCENE_NODE_KEYS[tkey]
    elif canon and canon in ALLOWED_SCENE_NODE_KEYS:
        allowed = ALLOWED_SCENE_NODE_KEYS[canon]
    else:
        # fallback to generic: allow common keys
        allowed = {"type", "visible_if", "action", "action_arg", "children", "x","y","w","h","rect","radius","r","stroke","fill","color","text","value","font","align","ellipsis","binding","source","name","icon","limit","item_width","item_height","gap","direction","layout"}
    for k in node:
        if k not in allowed:
            _err(f"scene node type '{raw_type}' unknown key '{k}' (allowed: {sorted(allowed)})")
    # visible_if and action handling -> flags
    flags = 0
    visible_binding = None
    action_id = None
    action_arg_binding = None
    if "visible_if" in node:
        vf = node["visible_if"]
        if not isinstance(vf, str) or not vf.startswith("$"):
            _err(f"visible_if must be binding string starting with $, got {vf!r}")
        visible_binding = _map_scene_binding(vf)
        flags |= 0x01
    if "action" in node:
        ac = node["action"]
        action_id = _map_scene_action(ac)
        flags |= 0x02
        if "action_arg" in node:
            arg = node["action_arg"]
            if not isinstance(arg, str) or not arg.startswith("$"):
                _err(f"action_arg must be binding string, got {arg!r}")
            action_arg_binding = _map_scene_binding(arg)
            # only open_app with $item.id is currently expected, but allow any item binding
            if ac != "open_app" and arg != "$item.id":
                # still allow but validate binding
                pass
        else:
            # open_app may have optional arg; others must not have arg but we already handle
            if "action_arg" in node:
                pass
    elif "action_arg" in node:
        _err(f"action_arg without action in node {raw_type}")
    # also need to handle interaction encoded via 'action' alternate key? Already handled
    # Build payload per type
    payload = bytearray()
    # prepend visible_if and action for flags order: visible_if first, then action
    if flags & 0x01:
        payload.extend(struct.pack("<B", visible_binding & 0xFF))
    if flags & 0x02:
        payload.extend(struct.pack("<B", action_id & 0xFF))
        if action_arg_binding is not None:
            payload.extend(struct.pack("<BB", 1, action_arg_binding & 0xFF))
        else:
            payload.extend(struct.pack("<B", 0))
    # type-specific
    if ntype == SCENE_NODE_CLEAR:
        color = node.get("color", "white")
        if not isinstance(color, str):
            _err("clear color must be string")
        c = color.lower()
        if c not in ("white", "black"):
            _err(f"clear color must be white or black, got {color!r}")
        payload.extend(struct.pack("<B", 0 if c=="white" else 1))
    elif ntype == SCENE_NODE_BITMAP:
        # optional bitmap: rect + source
        rect = _parse_rect_from_node(node, f"bitmap node")
        if rect is None:
            # bitmap may be full-screen implicitly? require rect
            _err("bitmap node requires rect or x/y/w/h")
        x,y,w,h = rect
        payload.extend(struct.pack("<HHHH", x, y, w, h))
        src = node.get("source")
        if src is not None:
            if not isinstance(src, str):
                _err("bitmap source must be string")
            payload.extend(_encode_scene_string(src))
        else:
            payload.extend(struct.pack("<H", 0))
    elif ntype == SCENE_NODE_LINE:
        # line: x,y,x2,y2,width
        for k in ("x","y","x2","y2"):
            if k not in node:
                _err(f"line node missing '{k}'")
            if not isinstance(node[k], int) or isinstance(node[k], bool):
                _err(f"line {k} must be int")
            if not (0 <= node[k] <= 800):
                # allow up to 800
                pass
        width = int(node.get("width", 1))
        if not isinstance(node.get("width", 1), int) or isinstance(node.get("width", 1), bool):
            _err("line width must be int")
        if not (1 <= width <= 8):
            _err(f"line width must be 1..8, got {width}")
        payload.extend(struct.pack("<HHHHB", node["x"]&0xFFFF, node["y"]&0xFFFF, node["x2"]&0xFFFF, node["y2"]&0xFFFF, width&0xFF))
        col = node.get("color", "black")
        if not isinstance(col, str):
            _err("line color must be string")
        payload.extend(struct.pack("<B", 0 if col.lower()=="white" else 1))
    elif ntype == SCENE_NODE_RECT:
        rect = _parse_rect_from_node(node, "rect node")
        if rect is None:
            _err("rect node requires rect or x/y/w/h")
        x,y,w,h = rect
        stroke = int(node.get("stroke", 0))
        if not isinstance(node.get("stroke",0), int) or isinstance(node.get("stroke",0), bool):
            _err("rect stroke must be int")
        if not (0 <= stroke <= 8):
            _err(f"rect stroke {stroke} out of 0..8")
        fill = 1 if node.get("fill", False) else 0
        if "fill" in node and not isinstance(node["fill"], bool):
            if node["fill"] not in (0,1):
                _err("rect fill must be bool")
        payload.extend(struct.pack("<HHHHBB", x,y,w,h, stroke&0xFF, fill&0xFF))
    elif ntype == SCENE_NODE_ROUND_RECT:
        rect = _parse_rect_from_node(node, "round_rect node")
        if rect is None:
            _err("round_rect node requires rect or x/y/w/h")
        x,y,w,h = rect
        r = node.get("r", node.get("radius", 0))
        if not isinstance(r, int) or isinstance(r,bool):
            _err("round_rect radius must be int")
        if not (0 <= r <= 64):
            _err(f"round_rect radius {r} out of 0..64")
        stroke = int(node.get("stroke", 0))
        if not (0 <= stroke <= 8):
            _err(f"round_rect stroke {stroke} out of 0..8")
        payload.extend(struct.pack("<HHHHHB", x,y,w,h, r&0xFFFF, stroke&0xFF))
        # optional fill bool
        fill = 1 if node.get("fill", False) else 0
        payload.extend(struct.pack("<B", fill&0xFF))
    elif ntype == SCENE_NODE_TEXT:
        rect = _parse_rect_from_node(node, "text node")
        if rect is None:
            _err("text node requires rect or x/y/w/h")
        x,y,w,h = rect
        # font
        font = node.get("font", "ui_16_regular")
        if not isinstance(font, str):
            _err("text font must be string")
        if font not in FONT_MAP:
            _err(f"unknown font '{font}' (allowed: {sorted(FONT_MAP)})")
        fid, fstyle = _map_font(font)
        align_s = node.get("align", "left")
        align = _map_align(align_s)
        ellipsis = 1 if node.get("ellipsis", True) else 0
        if "ellipsis" in node and not isinstance(node["ellipsis"], bool):
            _err("ellipsis must be bool")
        # text value: either 'text' or 'value'
        txt = node.get("text", node.get("value"))
        if txt is None:
            _err("text node missing 'text' or 'value'")
        if not isinstance(txt, str):
            _err("text value must be string")
        is_binding = 0
        binding_id = 0
        literal_bytes = b""
        if txt.startswith("$"):
            # binding
            binding_id = _map_scene_binding(txt)
            is_binding = 1
        else:
            literal_bytes = txt.encode("utf-8")
            if len(literal_bytes) > 255:
                _err("text literal too long >255")
        payload.extend(struct.pack("<HHHHBBBB", x,y,w,h, fid&0xFF, fstyle&0xFF, align&0xFF, ellipsis&0xFF))
        payload.extend(struct.pack("<BB", is_binding&0xFF, binding_id&0xFF))
        if is_binding:
            # no literal
            payload.extend(struct.pack("<H", 0))
        else:
            payload.extend(struct.pack("<H", len(literal_bytes)))
            payload.extend(literal_bytes)
    elif ntype in (SCENE_NODE_COVER,):
        rect = _parse_rect_from_node(node, "cover node")
        if rect is None:
            _err("cover node requires rect or x/y/w/h")
        x,y,w,h = rect
        r = node.get("r", node.get("radius", 0))
        if not isinstance(r, int) or isinstance(r,bool):
            _err("cover radius must be int")
        if not (0 <= r <= 64):
            _err(f"cover radius {r} out of 0..64")
        payload.extend(struct.pack("<HHHHH", x,y,w,h, r&0xFFFF))
        # binding for cover source
        binding = node.get("binding")
        if binding is not None:
            if not isinstance(binding, str) or not binding.startswith("$"):
                _err(f"cover binding must be binding string, got {binding!r}")
            bid = _map_scene_binding(binding)
            payload.extend(struct.pack("<BB", 1, bid &0xFF))
        else:
            payload.extend(struct.pack("<B", 0))
    elif ntype == SCENE_NODE_PROGRESS:
        rect = _parse_rect_from_node(node, "progress node")
        if rect is None:
            _err("progress node requires rect or x/y/w/h")
        x,y,w,h = rect
        r = node.get("r", node.get("radius", 0))
        if not isinstance(r, int) or isinstance(r,bool):
            _err("progress radius must be int")
        payload.extend(struct.pack("<HHHHH", x,y,w,h, r&0xFFFF))
        binding = node.get("binding", "$current.progress")
        if not isinstance(binding, str) or not binding.startswith("$"):
            _err(f"progress binding must be binding, got {binding!r}")
        bid = _map_scene_binding(binding)
        payload.extend(struct.pack("<B", bid &0xFF))
    elif ntype == SCENE_NODE_ICON:
        rect = _parse_rect_from_node(node, "icon node")
        if rect is None:
            _err("icon node requires rect or x/y/w/h")
        x,y,w,h = rect
        payload.extend(struct.pack("<HHHH", x,y,w,h))
        name = node.get("name", node.get("icon", ""))
        if not isinstance(name, str):
            _err("icon name must be string")
        payload.extend(_encode_scene_string(name))
        binding = node.get("binding")
        if binding is not None:
            if not isinstance(binding, str) or not binding.startswith("$"):
                _err(f"icon binding must be binding, got {binding!r}")
            bid = _map_scene_binding(binding)
            payload.extend(struct.pack("<BB", 1, bid&0xFF))
        else:
            payload.extend(struct.pack("<B", 0))
    elif ntype == SCENE_NODE_BATTERY:
        rect = _parse_rect_from_node(node, "battery node")
        if rect is None:
            # battery may have x/y/w/h
            _err("battery node requires rect or x/y/w/h")
        x,y,w,h = rect
        payload.extend(struct.pack("<HHHH", x,y,w,h))
        binding = node.get("binding", "$system.battery")
        if not isinstance(binding, str) or not binding.startswith("$"):
            _err(f"battery binding must be binding, got {binding!r}")
        bid = _map_scene_binding(binding)
        payload.extend(struct.pack("<B", bid&0xFF))
    elif ntype == SCENE_NODE_GROUP:
        children = node.get("children")
        if not isinstance(children, list):
            _err("group node requires children list")
        if len(children) > MAX_REPEAT_CHILDREN:
            _err(f"group children {len(children)} exceeds max {MAX_REPEAT_CHILDREN}")
        if len(children) == 0:
            _err("group children must not be empty")
        # encode children count then each child
        inner = bytearray()
        for ch in children:
            inner.extend(_encode_scene_node(ch, depth+1))
        payload.extend(struct.pack("<H", len(children) &0xFFFF))
        payload.extend(inner)
    elif ntype == SCENE_NODE_REPEAT:
        source = node.get("source")
        if not isinstance(source, str) or not source.startswith("$"):
            _err(f"repeat source must be binding string starting with $, got {source!r}")
        # Validate against effective binding map (generic page lists, not hard-coded Home only)
        sid = _map_scene_binding(source)
        limit = int(node.get("limit", 3))
        if not isinstance(node.get("limit", 3), int) or isinstance(node.get("limit", 3), bool):
            _err("repeat limit must be int")
        if not (1 <= limit <= MAX_REPEAT_LIMIT):
            _err(f"repeat limit {limit} out of 1..{MAX_REPEAT_LIMIT}")
        # layout: x,y,item_width,item_height,gap,direction
        # Support either direct keys or layout object
        layout = node.get("layout", {})
        if not isinstance(layout, dict):
            _err("repeat layout must be object")
        # allow top-level keys as fallback
        x = node.get("x", layout.get("x"))
        y = node.get("y", layout.get("y"))
        iw = node.get("item_width", layout.get("item_width", layout.get("itemWidth")))
        ih = node.get("item_height", layout.get("item_height", layout.get("itemHeight")))
        gap = node.get("gap", layout.get("gap", 0))
        direction = node.get("direction", layout.get("direction", "horizontal"))
        if None in (x,y,iw,ih):
            _err(f"repeat requires x,y,item_width,item_height")
        for v,name in [(x,"x"),(y,"y"),(iw,"item_width"),(ih,"item_height"),(gap,"gap")]:
            if not isinstance(v,int) or isinstance(v,bool):
                _err(f"repeat {name} must be int")
        if direction not in ("horizontal","vertical"):
            _err(f"repeat direction must be horizontal or vertical, got {direction!r}")
        dir_val = 0 if direction=="horizontal" else 1
        children = node.get("children")
        if not isinstance(children, list) or len(children)==0:
            _err("repeat requires non-empty children list")
        if len(children) > MAX_REPEAT_CHILDREN:
            _err(f"repeat children {len(children)} exceeds max {MAX_REPEAT_CHILDREN}")
        inner = bytearray()
        for ch in children:
            inner.extend(_encode_scene_node(ch, depth+1))
        payload.extend(struct.pack("<BBHHHHH", sid&0xFF, limit&0xFF, x&0xFFFF, y&0xFFFF, iw&0xFFFF, ih&0xFFFF, gap&0xFFFF))
        payload.extend(struct.pack("<BBH", dir_val&0xFF, 0, len(children)&0xFFFF))
        payload.extend(inner)
    else:
        _err(f"unsupported scene node type {ntype}")
    # Build command header: type, flags, payload_len
    header = struct.pack("<BBH", ntype &0xFF, flags &0xFF, len(payload) &0xFFFF)
    data = header + payload
    # Pad to 4-byte alignment
    pad = (4 - (len(data) % 4)) % 4
    if pad:
        data += b"\x00" * pad
    return data

def _compile_scene(nodes: list, theme_dir: Path) -> bytes:
    if not isinstance(nodes, list):
        _err("nodes must be list")
    if len(nodes) == 0:
        _err("nodes must not be empty")
    if len(nodes) > MAX_SCENE_NODES:
        _err(f"nodes count {len(nodes)} exceeds max {MAX_SCENE_NODES}")
    cmds = b"".join(_encode_scene_node(n, depth=0) for n in nodes)
    # Scene section header: version, command_count, flags, reserved
    header = struct.pack("<HHHH", SCENE_VERSION, len(nodes) &0xFFFF, 0, 0)
    return header + cmds

def decode_scene(data: bytes):
    """Host decode for SCENE section: returns list of dicts with type and order."""
    if len(data) < 8:
        raise ValueError("scene data too small")
    ver, count, flags, res = struct.unpack("<HHHH", data[:8])
    if ver != SCENE_VERSION:
        raise ValueError(f"scene version {ver} !=1")
    off = 8
    out = []
    # canonical reverse: prefer "cover" over "image" for type 6, etc
    reverse_map = {}
    for k,v in SCENE_NODE_TYPE_MAP.items():
        if v not in reverse_map:
            reverse_map[v] = k
    # ensure cover preferred
    if 6 in reverse_map:
        reverse_map[6] = "cover"
    binding_rev = {v:k for k,v in BINDING_SCENE_MAP.items()}
    action_rev = {v:k for k,v in ACTION_SCENE_MAP.items()}
    for i in range(count):
        if off + 4 > len(data):
            raise ValueError("truncated scene cmd header")
        ntype, flags, plen = struct.unpack("<BBH", data[off:off+4])
        off += 4
        if off + plen > len(data):
            raise ValueError("truncated scene payload")
        payload = data[off:off+plen]
        # record type name
        tname = reverse_map.get(ntype, f"unknown_{ntype}")
        entry = {"type": tname, "raw_type": ntype, "flags": flags, "payload": payload}
        # crude parse for test visibility: extract literal text if present for text nodes
        if ntype == SCENE_NODE_TEXT:
            try:
                # payload layout for text: visible_if? action? then x,y,w,h,fid,fstyle,align,ellipsis,is_binding,binding_id,text_len,bytes
                # we have variable prefix for visible/action, but for minimal case flags=0 we can parse fixed
                p_off = 0
                if flags & 0x01:
                    p_off += 1
                if flags & 0x02:
                    # action_id + has_arg + maybe arg
                    has_arg = payload[p_off+1]
                    p_off += 2 + (1 if has_arg else 0)
                if len(payload) >= p_off+ 8+4:
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    entry["rect"] = [x,y,w,h]
                    p_off+=8
                    fid,fstyle,align,ellipsis = struct.unpack("<BBBB", payload[p_off:p_off+4])
                    p_off+=4
                    is_binding, bid = struct.unpack("<BB", payload[p_off:p_off+2])
                    p_off+=2
                    tlen = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    if tlen>0 and p_off+tlen<=len(payload):
                        entry["text"] = payload[p_off:p_off+tlen].decode("utf-8", errors="replace")
                        entry["is_binding"] = bool(is_binding)
                        if is_binding:
                            entry["binding"] = binding_rev.get(bid, bid)
                    else:
                        if is_binding:
                            entry["binding"] = binding_rev.get(bid, bid)
                            entry["is_binding"] = True
            except Exception:
                pass
        elif ntype == SCENE_NODE_CLEAR:
            try:
                p_off=0
                if flags &0x01:
                    p_off+=1
                if flags &0x02:
                    has_arg = payload[p_off+1]
                    p_off+=2 + (1 if has_arg else 0)
                if len(payload)>p_off:
                    entry["color"] = "white" if payload[p_off]==0 else "black"
            except: pass
        elif ntype == SCENE_NODE_LINE:
            try:
                p_off=0
                if flags &0x01:
                    p_off+=1
                if flags &0x02:
                    has_arg = payload[p_off+1]
                    p_off+=2 + (1 if has_arg else 0)
                if len(payload)>=p_off+10:
                    x,y,x2,y2 = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    w=payload[p_off+8]
                    entry["rect"]=[x,y,x2,y2]
                    entry["width"]=w
            except: pass
        elif ntype == SCENE_NODE_REPEAT:
            try:
                p_off=0
                if flags &0x01:
                    p_off+=1
                if flags &0x02:
                    has_arg = payload[p_off+1]
                    p_off+=2 + (1 if has_arg else 0)
                if len(payload)>=p_off+8:
                    sid,lim,x,y,iw,ih,gap = struct.unpack("<BBHHHHH", payload[p_off:p_off+12]) if len(payload)>=p_off+12 else (0,0,0,0,0,0,0)
                    # use binding_rev for source
                    entry["source"] = binding_rev.get(sid, sid)
                    entry["limit"]=lim
            except: pass
        off += plen
        # skip padding
        pad = (4 - (plen %4))%4 if False else 0
        # Actually we padded whole command, not just payload, so after payload we need to skip padding to align next header
        # header+payload was padded to 4, so off should be aligned
        aligned = (off + 3)//4*4
        # but we already moved off by plen, and previous off was at payload start, so total header+payload =4+plen, padded to multiple of 4
        # So compute expected off after padding
        # off currently points after payload, but we started payload after header, so total consumed =4+plen padded
        # easiest: align off to 4
        off = (off + 3)//4*4 if (4+plen)%4!=0 else off
        # However if we already did p_off parsing, we lost plen, simpler: off is already after payload, align it
        # We'll just align
        out.append(entry)
    return out


def _load_and_pack_background(theme_dir: Path, bg_cfg: dict) -> bytes:
    # Pack the theme background into the M4TH 1-bpp asset.
    # threshold (legacy/default): resize + grayscale threshold.
    # prebinarized_alpha: authored 480x800 PNG mask; no resampling and no
    # luminance threshold. Transparent or pure-white pixels are white, while
    # any visible non-white authored ink is black.
    if not isinstance(bg_cfg, dict):
        _err("background must be object")
    _require_keys(bg_cfg, ["source", "source_size", "fit"], "background")
    _reject_unknown_keys(bg_cfg, ALLOWED_BG_KEYS, "background")

    src_rel = bg_cfg["source"]
    if not isinstance(src_rel, str):
        _err("background.source must be string")

    source_size = bg_cfg["source_size"]
    if not isinstance(source_size, list) or len(source_size) != 2:
        _err("background.source_size must be [w,h]")
    sw, sh = source_size
    if (not isinstance(sw, int) or isinstance(sw, bool) or
            not isinstance(sh, int) or isinstance(sh, bool)):
        _err("background.source_size must be ints")
    if sw <= 0 or sh <= 0:
        _err("background.source_size must be >0")

    mode = bg_cfg.get("mode", "threshold")
    if mode not in ("threshold", "prebinarized_alpha"):
        _err(f"background.mode must be 'threshold' or 'prebinarized_alpha', got {mode!r}")

    fit = bg_cfg["fit"]
    if mode == "threshold":
        if fit != "stretch_to_screen":
            _err(f"background.fit must be 'stretch_to_screen' in threshold mode, got {fit!r}")
        _require_keys(bg_cfg, ["threshold", "erase_regions"], "background")
        threshold = bg_cfg["threshold"]
        if not isinstance(threshold, int) or isinstance(threshold, bool) or not (0 <= threshold <= 255):
            _err(f"background.threshold must be int 0..255, got {threshold!r}")
        erase_regions = bg_cfg["erase_regions"]
    else:
        if fit != "native":
            _err(f"background.fit must be 'native' in prebinarized_alpha mode, got {fit!r}")
        if "threshold" in bg_cfg:
            _err("background.threshold is forbidden in prebinarized_alpha mode")
        if source_size != [SCREEN_W, SCREEN_H]:
            _err(
                f"prebinarized_alpha background must already be 480x800; "
                f"source_size is {source_size}"
            )
        erase_regions = bg_cfg.get("erase_regions", [])

    if not isinstance(erase_regions, list):
        _err("background.erase_regions must be list")
    for i, r in enumerate(erase_regions):
        _validate_rect(r, f"erase_regions[{i}]")

    src_path = (theme_dir / src_rel).resolve()
    if not src_path.is_file():
        _err(f"background source not found: {src_rel} (resolved {src_path})")

    try:
        from PIL import Image
    except ImportError as e:
        _err(f"Pillow required for background conversion: {e}. Install with pip install Pillow")

    im = Image.open(src_path)
    actual_w, actual_h = im.size
    if [actual_w, actual_h] != source_size:
        _err(
            f"background source_size {source_size} does not match actual image "
            f"dimensions [{actual_w},{actual_h}]"
        )

    dst_w, dst_h = SCREEN_W, SCREEN_H
    stride = (dst_w + 7) // 8
    assert stride == 60
    data = bytearray(stride * dst_h)

    if mode == "prebinarized_alpha":
        if im.size != (dst_w, dst_h):
            _err(
                f"prebinarized_alpha background must already be 480x800; "
                f"actual image is {im.size[0]}x{im.size[1]}"
            )
        px = im.convert("RGBA").load()
        for y in range(dst_h):
            row_off = y * stride
            for x in range(dst_w):
                r, g, b, alpha = px[x, y]
                # Alpha is an authored cut-out mask. Never apply a luminance threshold here.
                if alpha != 0 and (r != 255 or g != 255 or b != 255):
                    data[row_off + (x // 8)] |= (1 << (7 - (x % 8)))
    else:
        # Legacy reference-image pipeline stays backward compatible.
        if theme_dir.name == "mofei-classic" and source_size != [919, 1536]:
            _err(f"mofei-classic threshold source_size must be [919,1536], got {source_size}")
        if im.mode != "RGB":
            im = im.convert("RGB")
        if im.size != (dst_w, dst_h):
            im = im.resize((dst_w, dst_h), Image.LANCZOS)
        px = im.convert("L").load()
        for y in range(dst_h):
            row_off = y * stride
            for x in range(dst_w):
                if px[x, y] < threshold:
                    data[row_off + (x // 8)] |= (1 << (7 - (x % 8)))

    for r in erase_regions:
        ex, ey, ew, eh = r
        for yy in range(ey, ey + eh):
            if not (0 <= yy < dst_h):
                continue
            row_off = yy * stride
            for xx in range(ex, ex + ew):
                if not (0 <= xx < dst_w):
                    continue
                byte_idx = row_off + (xx // 8)
                bit = 7 - (xx % 8)
                data[byte_idx] &= ~(1 << bit)

    assert len(data) == 48000
    return bytes(data)

def compile_theme(theme_path: Path, out_path: Path, header_path: Path | None = None) -> bytes:
    theme_path = Path(theme_path).resolve()
    if not theme_path.is_file():
        _err(f"theme file not found: {theme_path}")
    theme_dir = theme_path.parent
    raw = theme_path.read_text(encoding="utf-8")
    try:
        cfg = json.loads(raw)
    except json.JSONDecodeError as e:
        _err(f"theme.json invalid JSON: {e}")
    if not isinstance(cfg, dict):
        _err("theme.json top level must be object")
    _reject_unknown_keys(cfg, ALLOWED_TOP_KEYS, "theme")
    _require_keys(cfg, ["format", "id", "screen"], "theme")
    has_nodes = "nodes" in cfg
    has_slots = "slots" in cfg
    has_bg = "background" in cfg
    if not has_nodes and not has_slots:
        _err("theme must have 'nodes' or 'slots' (legacy)")
    if has_nodes:
        if not isinstance(cfg["nodes"], list):
            _err("nodes must be list")
        # nodes path allows missing background/slots; they become optional synthetic
    else:
        _require_keys(cfg, ["background", "slots"], "theme")
    if cfg["format"] != 1:
        _err(f"theme format must be 1, got {cfg['format']!r}")
    theme_id = cfg["id"]
    if not isinstance(theme_id, str) or not theme_id:
        _err("theme id must be non-empty string")
    if not theme_id.replace("-", "").replace("_", "").isalnum():
        _err(f"theme id '{theme_id}' must be alnum with -_")
    screen = cfg["screen"]
    if not isinstance(screen, list) or len(screen) != 2:
        _err("screen must be [480,800]")
    if screen != [SCREEN_W, SCREEN_H]:
        _err(f"screen must be [480,800], got {screen!r}")
    # Legacy mofei-classic references used threshold 128. Authored alpha templates bypass it.
    bg_cfg = cfg.get("background") if has_bg else None
    if theme_id == "mofei-classic" and bg_cfg is not None and bg_cfg.get("mode", "threshold") == "threshold":
        if bg_cfg.get("threshold") != 128:
            _err(f"mofei-classic threshold must be 128 (not {bg_cfg.get('threshold')!r}), spec example 170 is illustrative")
    # Manifest bindings/actions: optional theme dictionaries merged with reserved common
    bindings_manifest = cfg.get("bindings")
    actions_manifest = cfg.get("actions")
    global _ACTIVE_SCENE_BINDING_MAP, _ACTIVE_SCENE_ACTION_MAP
    if bindings_manifest is None and actions_manifest is None:
        _ACTIVE_SCENE_BINDING_MAP = dict(BINDING_SCENE_MAP)
        _ACTIVE_SCENE_ACTION_MAP = dict(ACTION_SCENE_MAP)
    else:
        # Build effective maps: common + legacy + manifest (manifest adds page-specific, validated against reserved)
        effective_bindings: dict = dict(COMMON_SCENE_BINDINGS)
        effective_bindings.update(LEGACY_SCENE_BINDINGS)
        effective_actions: dict = dict(LEGACY_SCENE_ACTIONS)
        if bindings_manifest is not None:
            if not isinstance(bindings_manifest, dict):
                _err("bindings must be object")
            manifest_ids_seen: dict[int, str] = {}
            for k, v in bindings_manifest.items():
                if not isinstance(k, str):
                    _err(f"binding key must be string, got {k!r}")
                if not k.startswith("$"):
                    _err(f"binding '{k}' must start with $")
                if k in RESERVED_SCENE_BINDING_NAMES:
                    _err(f"binding '{k}' collides with reserved common/legacy (reserved)")
                if isinstance(v, bool):
                    _err(f"binding '{k}' id must be int 1..254, got bool")
                if not isinstance(v, int):
                    _err(f"binding '{k}' id must be int 1..254, got {v!r}")
                if not (1 <= v <= 254):
                    _err(f"binding '{k}' id {v} out of range 1..254")
                if v in RESERVED_SCENE_BINDING_IDS:
                    _err(f"binding '{k}' id {v} collides with reserved binding id (reserved)")
                if v in manifest_ids_seen:
                    _err(f"duplicate binding id {v} (bindings '{manifest_ids_seen[v]}' and '{k}')")
                manifest_ids_seen[v] = k
                effective_bindings[k] = v
        if actions_manifest is not None:
            if not isinstance(actions_manifest, dict):
                _err("actions must be object")
            manifest_aids_seen: dict[int, str] = {}
            for k, v in actions_manifest.items():
                if not isinstance(k, str):
                    _err(f"action key must be string, got {k!r}")
                if not k:
                    _err("action key must be non-empty string")
                if k in RESERVED_SCENE_ACTION_NAMES:
                    _err(f"action '{k}' collides with reserved action (reserved)")
                if isinstance(v, bool):
                    _err(f"action '{k}' id must be int 0..254, got bool")
                if not isinstance(v, int):
                    _err(f"action '{k}' id must be int 0..254, got {v!r}")
                if not (0 <= v <= 254):
                    _err(f"action '{k}' id {v} out of range 0..254")
                if v in RESERVED_SCENE_ACTION_IDS:
                    _err(f"action '{k}' id {v} collides with reserved action id (reserved)")
                if v in manifest_aids_seen:
                    _err(f"duplicate action id {v} (actions '{manifest_aids_seen[v]}' and '{k}')")
                manifest_aids_seen[v] = k
                effective_actions[k] = v
        _ACTIVE_SCENE_BINDING_MAP = effective_bindings
        _ACTIVE_SCENE_ACTION_MAP = effective_actions
    # Slots handling: legacy mandatory, scene optional synthetic
    slots = cfg.get("slots", [])
    if has_slots:
        if not isinstance(slots, list):
            _err("slots must be list")
        if len(slots) > MAX_SLOTS:
            _err(f"slots count {len(slots)} exceeds max {MAX_SLOTS}")
        if len(slots) == 0:
            _err("slots must not be empty")
        seen_ids = set()
        focus_orders = {}
        for s in slots:
            if not isinstance(s, dict):
                _err(f"each slot must be object, got {s!r}")
            _require_keys(s, ["id", "type", "rect"], f"slot")
            sid = s["id"]
            if not isinstance(sid, str) or not sid:
                _err("slot id must be non-empty string")
            if sid in seen_ids:
                _err(f"duplicate slot id '{sid}'")
            seen_ids.add(sid)
            # strict bool check for focusable
            if "focusable" in s and not isinstance(s["focusable"], bool):
                _err(f"slot '{sid}' focusable must be bool")
            if s.get("focusable", False):
                fo = s.get("focus_order")
                if fo is None:
                    _err(f"slot '{sid}' focusable requires focus_order")
                if not isinstance(fo, int) or isinstance(fo, bool) or not (0 <= fo <= 63):
                    _err(f"slot '{sid}' focus_order must be int 0..63")
                if fo in focus_orders:
                    _err(f"duplicate focus_order {fo} (slots '{focus_orders[fo]}' and '{sid}')")
                focus_orders[fo] = sid
            else:
                if "focus_order" in s:
                    _err(f"slot '{sid}' non-focusable must not have focus_order")
        slot_blobs = []
        for s in slots:
            blob = _pack_slot(s)
            slot_blobs.append(blob)
        slots_data = b"".join(slot_blobs)
    else:
        # Synthetic empty slots for scene-only packs (keeps legacy decoder happy)
        slots_data = b""
        slots = []
    id_bytes = theme_id.encode("utf-8") + b"\x00"
    if len(id_bytes) > MAX_STRINGS:
        _err(f"strings payload {len(id_bytes)} exceeds {MAX_STRINGS}")
    strings_data = id_bytes
    meta_data = struct.pack("<HHHHII", VERSION, HEADER_SIZE, SCREEN_W, SCREEN_H, 0, 0)
    assert len(meta_data) == META_SIZE
    if has_bg:
        bg_data = _load_and_pack_background(theme_dir, bg_cfg)
    else:
        # White fallback when scene has no bitmap (no legacy background)
        bg_data = bytes(48000)
    assert len(bg_data) == 48000
    # Compile scene if present
    scene_data = None
    if has_nodes:
        scene_data = _compile_scene(cfg["nodes"], theme_dir)
        if len(scene_data) > 32*1024:
            _err(f"scene payload {len(scene_data)} exceeds 32KiB")
    # Build sections: legacy 5 always, plus optional SCENE
    base_sections = [(SECTION_META, meta_data, 0), (SECTION_STRINGS, strings_data, 0), (SECTION_SLOTS, slots_data, len(slots)), (SECTION_ASSETS, b"", 1), (SECTION_ASSET_DATA, bg_data, 1)]
    raw_sections = base_sections[:]
    if scene_data is not None:
        raw_sections.append((SECTION_SCENE, scene_data, len(cfg["nodes"])))
    section_count = len(raw_sections)
    base_payload_off = HEADER_SIZE + section_count * 24
    dummy_assets_data = _pack_asset_record(0, SCREEN_W, SCREEN_H, 60, 0, len(bg_data))
    raw_sections[3] = (SECTION_ASSETS, dummy_assets_data, 1)
    offsets = []
    lengths = []
    counts = []
    cur = base_payload_off
    for typ, data, cnt in raw_sections:
        cur = _align_up(cur, 4)
        offsets.append(cur)
        lengths.append(len(data))
        counts.append(cnt)
        cur += len(data)
    bg_offset = offsets[4]
    real_assets_data = _pack_asset_record(0, SCREEN_W, SCREEN_H, 60, bg_offset, len(bg_data))
    raw_sections[3] = (SECTION_ASSETS, real_assets_data, 1)
    assert len(real_assets_data) == len(dummy_assets_data)
    offsets = []
    lengths = []
    counts = []
    cur = base_payload_off
    for typ, data, cnt in raw_sections:
        cur = _align_up(cur, 4)
        offsets.append(cur)
        lengths.append(len(data))
        counts.append(cnt)
        cur += len(data)
    total_size = _align_up(cur, 4)
    header_without_crc = struct.pack("<4sHHIHHHHI8s", MAGIC, VERSION, HEADER_SIZE, total_size, SCREEN_W, SCREEN_H, section_count, 0, 0, b"\x00" * 8)
    assert len(header_without_crc) == 32
    table_bytes = bytearray()
    for i, (typ, data, cnt) in enumerate(raw_sections):
        desc = struct.pack("<IIIIII", typ, 0, offsets[i], lengths[i], counts[i], 0)
        table_bytes.extend(desc)
    assert len(table_bytes) == section_count * 24
    file_bytes = bytearray(total_size)
    file_bytes[0:32] = header_without_crc
    file_bytes[32:32+len(table_bytes)] = table_bytes
    for i, (typ, data, cnt) in enumerate(raw_sections):
        off = offsets[i]
        file_bytes[off:off+len(data)] = data
    payload = bytes(file_bytes[32:])
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    header_with_crc = struct.pack("<4sHHIHHHHI8s", MAGIC, VERSION, HEADER_SIZE, total_size, SCREEN_W, SCREEN_H, section_count, 0, crc, b"\x00" * 8)
    file_bytes[0:32] = header_with_crc
    out = bytes(file_bytes)
    if len(out) > MAX_TOTAL:
        _err(f"pack total {len(out)} exceeds {MAX_TOTAL}")
    if len(strings_data) > MAX_STRINGS:
        _err(f"strings {len(strings_data)} exceeds {MAX_STRINGS}")
    out_path = Path(out_path).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)
    if header_path is not None:
        header_path = Path(header_path).resolve()
        header_path.parent.mkdir(parents=True, exist_ok=True)
        var_name = theme_id.replace("-", "_").replace(" ", "_")
        if not var_name[0].isalpha():
            var_name = "theme_" + var_name
        var_name = var_name + "_m4theme"
        hex_lines = []
        for i in range(0, len(out), 12):
            chunk = out[i:i+12]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            hex_lines.append(f"  {hex_str},")
        hex_body = "\n".join(hex_lines)
        header_content = f"""#pragma once
#include <cstdint>
#include <avr/pgmspace.h>

constexpr uint32_t {var_name}_len = {len(out)}u;
constexpr uint8_t {var_name}[] PROGMEM = {{
{hex_body}
}};
"""
        header_path.write_text(header_content, encoding="utf-8")
    return out

def validate_m4theme(data: bytes) -> tuple[bool, str]:
    """Validate M4TH v1 pack: header, section table bounds, non-overlap, CRC, record bounds."""
    if len(data) < HEADER_SIZE:
        return False, "too small"
    try:
        magic, ver, hs, total, sw, sh, sc, flags, crc = struct.unpack("<4sHHIHHHHI", data[:24])
        reserved = data[24:32]
    except struct.error:
        return False, "header unpack failed"
    if magic != MAGIC:
        return False, f"bad magic {magic!r}"
    if ver != VERSION:
        return False, f"bad version {ver}"
    if hs != HEADER_SIZE:
        return False, f"bad header_size {hs}"
    if total != len(data):
        return False, f"total_size {total} != file len {len(data)}"
    if sw != SCREEN_W or sh != SCREEN_H:
        return False, f"bad resolution {sw}x{sh}"
    if sc not in (5,6,7):
        return False, f"bad section count {sc} (expected 5 legacy or 6/7 with scene)"
    if flags != 0:
        return False, f"bad flags {flags}"
    if reserved != b"\x00"*8:
        return False, "reserved not zero"
    if len(data) < 32 + sc*24:
        return False, "truncated section table"
    calc_crc = binascii.crc32(data[32:]) & 0xFFFFFFFF
    if crc != calc_crc:
        return False, f"crc mismatch header {crc:08x} != calc {calc_crc:08x}"
    sections = []
    for i in range(sc):
        off = 32 + i*24
        typ, fl, ofs, ln, cnt, cc = struct.unpack("<IIIIII", data[off:off+24])
        if typ not in (1,2,3,4,5,6,7):
            return False, f"unknown section type {typ}"
        if fl != 0 or cc != 0:
            return False, f"section {typ} flags/crc must be 0"
        if ofs %4 !=0:
            return False, f"section {typ} offset not 4-aligned"
        if ofs + ln > len(data):
            return False, f"section {typ} overflow"
        if cnt > 64 and typ==3:
            return False, f"too many slots {cnt}"
        sections.append((typ, ofs, ln, cnt))
    # strict non-overlap (allow zero-length sections to share offset)
    sorted_secs = sorted(sections, key=lambda x: x[1])
    for i in range(len(sorted_secs)-1):
        cur_typ, cur_ofs, cur_ln, cur_cnt = sorted_secs[i]
        nxt_typ, nxt_ofs, nxt_ln, nxt_cnt = sorted_secs[i+1]
        if cur_ofs + cur_ln > nxt_ofs:
            return False, f"sections {cur_typ} and {nxt_typ} overlap"
        if cur_ofs == nxt_ofs and cur_ln != 0 and nxt_ln != 0:
            return False, "duplicate offset"
    # check slot record bounds
    for typ, ofs, ln, cnt in sections:
        if typ == 3:
            if ln != cnt * SLOT_SIZE:
                return False, f"slots length {ln} != cnt*32"
            if cnt > MAX_SLOTS:
                return False, "too many slots"
        if typ == 4:
            if ln != cnt * ASSET_RECORD_SIZE:
                return False, "asset record size mismatch"
        if typ == 5:
            if ln != 48000:
                return False, f"asset_data len {ln} !=48000"
        if typ == 6:
            if ln < 8:
                return False, f"scene length {ln} too small"
            if cnt > MAX_SCENE_NODES:
                return False, f"scene count {cnt} too large"
    return True, "ok"

def main() -> int:
    p = argparse.ArgumentParser(description="Compile M4 Home theme JSON to .m4theme")
    p.add_argument("--theme", required=True, type=Path, help="path to theme.json")
    p.add_argument("--out", required=True, type=Path, help="output .m4theme path")
    p.add_argument("--emit-header", type=Path, default=None, help="optional output C header path")
    args = p.parse_args()
    try:
        out_bytes = compile_theme(args.theme, args.out, args.emit_header)
        print(f"compiled {args.theme} -> {args.out} ({len(out_bytes)} bytes, CRC32={binascii.crc32(out_bytes[32:]) & 0xFFFFFFFF:08x})")
        if args.emit_header:
            print(f"emitted header {args.emit_header}")
        return 0
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
