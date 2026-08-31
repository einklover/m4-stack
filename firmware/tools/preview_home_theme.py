#!/usr/bin/env python3
"""
Host-side preview renderer for M4 Home theme .m4theme packs.

Reads the actual compiled build/mofei-classic.m4theme (M4TH v1), decodes its
binary sections, and renders a 480x800 preview using placeholder/sample dynamic
data.

Layering (spec 9):
  1. background (streamed 1bpp raster)
  2. dynamic slots (covers, text, progress, wifi, quick entries, bottom nav)
  3. cover corner masks/borders (procedural rounded clip + 1px border)
  4. focus/selection

Geometry is taken from the pack's slot table, not from theme.json or HomeRef.

Fonts: device uses embedded NotoSans / M4 UI bitmap faces via EpdFont.
Host preview uses the closest available system CJK font (Hiragino Sans GB /
STHeiti / Songti / Arial Unicode) with matched pixel sizes. The chosen host
fonts are printed explicitly.

Usage:
  python3 firmware/tools/preview_home_theme.py
  python3 firmware/tools/preview_home_theme.py --m4theme build/mofei-classic.m4theme --out tmp-home-screenshots/preview-mofei-classic.png
  python3 firmware/tools/preview_home_theme.py --focus-order 0
"""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
from pathlib import Path
from dataclasses import dataclass

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_M4THEME = ROOT / "build" / "mofei-classic.m4theme"
DEFAULT_OUT = ROOT / "tmp-home-screenshots" / "preview-mofei-classic.png"

MAGIC = b"M4TH"
VERSION = 1
HEADER_SIZE = 32
SCREEN_W = 480
SCREEN_H = 800
STRIDE = 60
BG_LEN = 48000

SECTION_META = 1
SECTION_STRINGS = 2
SECTION_SLOTS = 3
SECTION_ASSETS = 4
SECTION_ASSET_DATA = 5
SECTION_SCENE = 6
SECTION_INTERACTIONS = 7

# font candidates mirror m4ui_preview.py but narrowed to host-available CJK
FONT_CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]
BOLD_CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Arial Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]

# Mapping for preview font sizes. Device uses bitmap fonts sized per HomeRef;
# host preview approximates at same pixel heights. The mapping from compile's
# font_id (0..13) to approximate px is explicit here.
# 0 ui_12_regular, 1 ui_12_bold, etc. We map bold flag separately.
# Stable font IDs from compile_home_theme.py FONT_MAP (locked, not temporary ordinals)
# 10 ui_10_regular, 11 ui_10_bold, 12 ui_12_regular, 13 ui_12_bold, etc.
FONT_ID_TO_PX = {
    10: 15,  # ui_10_regular
    11: 15,  # ui_10_bold (same px, bold style)
    12: 15,  # ui_12_regular -> ~15-16
    13: 18,  # ui_12_bold -> hero title slightly larger
    14: 16,  # ui_14_regular
    15: 16,  # ui_14_bold
    16: 17,  # ui_16_regular
    17: 17,  # ui_16_bold
    18: 18,  # ui_18_regular
    19: 18,  # ui_18_bold
    20: 19,  # ui_20_regular
    21: 19,  # ui_20_bold
    22: 20,  # ui_22_regular
    23: 20,  # ui_22_bold
    24: 22,  # ui_24_regular
    25: 22,  # ui_24_bold
    30: 13,  # small_regular
    31: 13,  # small_bold
    # fallback for old temporary ordinals (0..13) to keep backward compat with old packs
    0: 15,
    1: 18,
    2: 16,
    3: 16,
    4: 17,
    5: 17,
    6: 18,
    7: 18,
    8: 19,
    9: 19,
}
FALLBACK_PX = 15
# Device note
FONT_FALLBACK_NOTE = (
    "Host preview note: real M4 device uses embedded NotoSans SC + Bookerly "
    "bitmap faces (EpdFont) rendered at 1bpp with Bayer dithering for muted "
    "text. This host preview uses the closest available TrueType host font "
    "(see --font candidates) rasterized via Pillow at matched pixel heights; "
    "antialiasing and metrics will differ slightly from the device bitmap."
)

@dataclass
class Slot:
    index: int
    stype: int  # 0 text,1 image/icon,2 cover,3 progress,4 hitbox
    binding: int
    target_kind: int
    target_idx: int
    target_action: int
    font_id: int
    font_style: int = 0xFF
    align: int = 0
    flags: int = 0
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    radius: int = 0
    stroke: int = 0
    focus_inset: int = 0
    focus_order: int = 0xFFFF
    asset_id: int = 0xFFFF
    string_off: int = 0xFFFF
    # derived
    focusable: bool = False

    @property
    def rect(self):
        return (self.x, self.y, self.w, self.h)

# sample dynamic data keyed by binding id
SAMPLE_TEXT = {
    2: "4 本",
    3: "长风渡",
    4: "墨书白 著",
    5: "晋江文学城 · 完结",
    8: "长安第一美人",
    13: "春日越轨",
    18: "我在古代开书局",
}
# overwrite for authors of minis if needed but theme only has title slots for minis
SAMPLE_PROGRESS = 38  # percent for hero
# cover glyphs per index
COVER_GLYPH = {
    0: "长",
    1: "长",
    2: "春",
    3: "我",
}
# quick entry / bottom nav labels keyed by action enum
ACTION_LABEL = {
    0: "文件",
    1: "微信读书",
    2: "番茄",
    3: "晋江",
    4: "历史",
    5: "应用",
    6: "设置",
}
# short labels for quick tiles (to fit tile width ~86px)
QUICK_SHORT = {
    0: "文件",
    1: "微信读书",
    2: "番茄",
    3: "晋江",
}


def first_existing(paths: list[str]) -> Path | None:
    for p in paths:
        cand = Path(p)
        if cand.exists():
            return cand.resolve()
    return None


def resolve_font_paths(font: Path | None, bold_font: Path | None) -> tuple[Path | None, Path | None, str]:
    note_parts = []
    if font:
        regular = Path(font).expanduser().resolve()
        if not regular.is_file():
            raise ValueError(f"--font does not exist: {regular}")
        bold = Path(bold_font).expanduser().resolve() if bold_font else regular
        font_note = f"Explicit host fonts: regular={regular} bold={bold}"
        return regular, bold, font_note
    # auto
    regular = first_existing(FONT_CANDIDATES)
    bold = first_existing(BOLD_CANDIDATES)
    if regular is None:
        note = "No CJK system font found; fallback to Pillow default bitmap (no Chinese)."
        return None, None, note
    if bold is None:
        bold = regular
    note = f"Host fonts (closest available): regular={regular} bold={bold}\n{FONT_FALLBACK_NOTE}"
    return regular, bold, note


def decode_m4theme(path: Path):
    data = Path(path).read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"file too small: {len(data)}")
    magic, ver, hs, total, sw, sh, sc, flags, crc = struct.unpack("<4sHHIHHHHI", data[:24])
    reserved = data[24:32]
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r} != M4TH")
    if ver != VERSION:
        raise ValueError(f"bad version {ver} != {VERSION}")
    if hs != HEADER_SIZE:
        raise ValueError(f"bad header_size {hs}")
    if total != len(data):
        raise ValueError(f"total_size {total} != file len {len(data)}")
    if sw != SCREEN_W or sh != SCREEN_H:
        raise ValueError(f"screen {sw}x{sh} != 480x800")
    if sc not in (5,6,7):
        raise ValueError(f"section_count {sc} not in 5/6/7")
    calc = binascii.crc32(data[32:]) & 0xFFFFFFFF
    if crc != calc:
        raise ValueError(f"CRC mismatch header {crc:08x} != calc {calc:08x}")
    # section table
    sections = {}
    off = 32
    for i in range(sc):
        typ, f, offset, length, count, scrc = struct.unpack("<IIIIII", data[off:off+24])
        off += 24
        if offset % 4 != 0:
            raise ValueError(f"section {typ} offset {offset} not 4-aligned")
        if offset + length > len(data):
            raise ValueError(f"section {typ} overflow")
        sections[typ] = {"type": typ, "flags": f, "offset": offset, "length": length, "count": count, "crc": scrc, "raw": data[offset:offset+length]}
    # verify required sections (5 legacy required, 6/7 optional)
    for required in [SECTION_META, SECTION_STRINGS, SECTION_SLOTS, SECTION_ASSETS, SECTION_ASSET_DATA]:
        if required not in sections:
            raise ValueError(f"missing section {required}")
    # optional SCENE
    scene = None
    scene_raw = None
    if SECTION_SCENE in sections:
        scene_raw = sections[SECTION_SCENE]["raw"]
        # decode via compile_home_theme if available, else minimal parse
        try:
            import importlib.util
            spec = importlib.util.spec_from_file_location("compile_home_theme_scene", str(ROOT / "firmware" / "tools" / "compile_home_theme.py"))
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)  # type: ignore
            if hasattr(mod, "decode_scene"):
                scene = mod.decode_scene(scene_raw)
            else:
                scene = None
        except Exception as e:
            # fallback minimal: decode count only
            try:
                ver, cnt, flags, res = struct.unpack("<HHHH", scene_raw[:8])
                scene = [{"type": f"raw_{i}"} for i in range(cnt)]
            except:
                scene = None
    # META
    meta_raw = sections[SECTION_META]["raw"]
    if len(meta_raw) != 16:
        raise ValueError(f"META length {len(meta_raw)} !=16")
    mver, mhs, msw, msh, mflags, mres = struct.unpack("<HHHHII", meta_raw)
    if mver != 1 or msw != 480 or msh != 800:
        raise ValueError(f"META content mismatch {mver} {msw}x{msh}")
    # STRINGS
    strings_raw = sections[SECTION_STRINGS]["raw"]
    # theme id null-terminated
    try:
        nul = strings_raw.index(b"\x00")
        theme_id = strings_raw[:nul].decode("utf-8")
    except ValueError:
        theme_id = strings_raw.decode("utf-8", errors="replace")
    # SLOTS - support both old (8B header) and new (9B+pad) layouts
    slots_raw = sections[SECTION_SLOTS]["raw"]
    cnt = sections[SECTION_SLOTS]["count"]
    if cnt * 32 != sections[SECTION_SLOTS]["length"]:
        raise ValueError(f"slots length mismatch {sections[SECTION_SLOTS]['length']} != {cnt}*32")

    def _try_parse_slots(data: bytes, use_new: bool) -> list[Slot] | None:
        out: list[Slot] = []
        for i in range(cnt):
            rec = data[i*32:(i+1)*32]
            try:
                if use_new:
                    # new layout: 9B header +1 pad +11H
                    if len(rec) < 32:
                        return None
                    stype, binding, tkind, tidx, tact, font_id, font_style, align, flags = struct.unpack("<BBBBBBBBB", rec[0:9])
                    pad = rec[9]
                    if pad != 0:
                        return None
                    x, y, w, h, radius, stroke, focus_inset, focus_order, asset_id, string_off, reserved = struct.unpack("<HHHHHHHHHHH", rec[10:32])
                    if reserved != 0:
                        return None
                    # basic bounds check for early rejection
                    if not (0 <= x < SCREEN_W and 0 <= y < SCREEN_H and w > 0 and h > 0 and x+w <= SCREEN_W and y+h <= SCREEN_H):
                        return None
                    if focus_order != 0xFFFF and not (0 <= focus_order <= 63):
                        return None
                    focusable = bool(flags & 0x01)
                    if not focusable and focus_order != 0xFFFF:
                        return None
                    out.append(Slot(i, stype, binding, tkind, tidx, tact, font_id, font_style, align, flags, x, y, w, h, radius, stroke, focus_inset, focus_order, asset_id, string_off, focusable))
                else:
                    stype, binding, tkind, tidx, tact, font_id, align, flags = struct.unpack("<BBBBBBBB", rec[0:8])
                    x, y, w, h, radius, fo, asset_id = struct.unpack("<HHHHHHH", rec[8:22])
                    string_off = struct.unpack("<H", rec[22:24])[0]
                    # old reserved check: last 8 bytes should be zero
                    if rec[24:32] != b"\x00"*8:
                        # allow but still check bounds
                        pass
                    if not (0 <= x < SCREEN_W and 0 <= y < SCREEN_H and w > 0 and h > 0 and x+w <= SCREEN_W and y+h <= SCREEN_H):
                        return None
                    if fo != 0xFFFF and not (0 <= fo <= 63):
                        return None
                    focusable = bool(flags & 0x01)
                    if not focusable and fo != 0xFFFF:
                        return None
                    out.append(Slot(i, stype, binding, tkind, tidx, tact, font_id, 0xFF, align, flags, x, y, w, h, radius, 0, 0, fo, asset_id, string_off, focusable))
            except Exception:
                return None
        return out

    slots = _try_parse_slots(slots_raw, use_new=True)
    if slots is None:
        slots = _try_parse_slots(slots_raw, use_new=False)
    if slots is None:
        # ultimate fallback: try old layout without validation (for corrupted but still decode)
        slots = []
        for i in range(cnt):
            rec = slots_raw[i*32:(i+1)*32]
            stype, binding, tkind, tidx, tact, font_id, align, flags = struct.unpack("<BBBBBBBB", rec[0:8])
            x, y, w, h, radius, fo, asset_id = struct.unpack("<HHHHHHH", rec[8:22])
            string_off = struct.unpack("<H", rec[22:24])[0]
            focusable = bool(flags & 1)
            slots.append(Slot(i, stype, binding, tkind, tidx, tact, font_id, 0xFF, align, flags, x, y, w, h, radius, 0, 0, fo, asset_id, string_off, focusable))
    # ASSETS
    assets_raw = sections[SECTION_ASSETS]["raw"]
    acnt = sections[SECTION_ASSETS]["count"]
    # Should be one asset
    assets = []
    for i in range(acnt):
        rec = assets_raw[i*24:(i+1)*24]
        aid, atype, aflags, aw, ah, stride, res, data_off, data_len, acrc = struct.unpack("<HBBHHHHIII", rec)
        assets.append({"id": aid, "type": atype, "width": aw, "height": ah, "stride": stride, "offset": data_off, "length": data_len})
    # ASSET_DATA background
    asset_data_raw = sections[SECTION_ASSET_DATA]["raw"]
    # The asset's offset/length should point into file; but asset_data_raw is that blob.
    # For validation, check asset offset matches section offset and length matches.
    # However compile stores exact offset file position; we can validate.
    # Use first asset's offset/length to slice background from file (more canonical)
    bg = None
    if assets:
        a = assets[0]
        bg = data[a["offset"]:a["offset"]+a["length"]]
        if len(bg) != BG_LEN:
            # fallback to section payload if offset mismatched (should not)
            bg = asset_data_raw
    else:
        bg = asset_data_raw
    if len(bg) != BG_LEN:
        raise ValueError(f"background length {len(bg)} != {BG_LEN}")
    # also background section count should be 1
    return {
        "header": {"magic": magic, "version": ver, "total": total, "crc": crc},
        "theme_id": theme_id,
        "slots": slots,
        "assets": assets,
        "background": bg,
        "sections": sections,
        "raw": data,
        "scene": scene,
        "scene_raw": scene_raw,
    }


def _px_for_font_id(fid: int) -> int:
    if fid == 0xFF or fid not in FONT_ID_TO_PX:
        return FALLBACK_PX
    return FONT_ID_TO_PX[fid]


def _is_bold_font_id(fid: int, style: int = 0xFF) -> bool:
    if style != 0xFF:
        return style == 1
    if fid == 0xFF:
        return False
    # mapping pattern: odd is bold for 0..13
    return (fid % 2) == 1 and fid < 14


def _font_for_slot(ImageFont, regular_path: Path | None, bold_path: Path | None, slot: Slot):
    fid = slot.font_id
    px = _px_for_font_id(fid)
    # hero title gets slightly larger to match HomeRef 24
    # we encode per-slot id heuristic when font_id is ui_12_bold but used for hero title
    # slot index 1 is hero_title
    if slot.index == 1 and fid == 13:
        px = 20
    elif slot.index in (2, 3): # hero author/source
        px = 14
    elif slot.index in (8, 9, 10): # mini titles center
        px = 13
    elif slot.index == 11: # recent count
        px = 13
    bold = _is_bold_font_id(fid, getattr(slot, 'font_style', 0xFF))
    # choose path
    chosen = bold_path if bold and bold_path else regular_path
    if chosen is None:
        # fallback to default load
        return ImageFont.load_default()
    try:
        # TTC needs index 0; Pillow will handle
        return ImageFont.truetype(str(chosen), px)
    except Exception:
        return ImageFont.load_default()


def _text_width(draw, text: str, font) -> int:
    try:
        # Pillow 8+
        return int(draw.textlength(text, font=font))
    except Exception:
        try:
            bbox = draw.textbbox((0, 0), text, font=font)
            return int(bbox[2] - bbox[0])
        except Exception:
            return len(text) * 6


def _fit_text(draw, text: str, font, max_w: int) -> str:
    if _text_width(draw, text, font) <= max_w:
        return text
    ellipsis = "…"
    if _text_width(draw, ellipsis, font) > max_w:
        return ""
    # binary shrink
    cur = text
    while cur and _text_width(draw, cur + ellipsis, font) > max_w:
        cur = cur[:-1]
    return cur + ellipsis


def _draw_wifi_glyph(draw, x: int, y: int, w: int, h: int):
    # wifi icon similar to FengyanTheme::drawWifiGlyph scaled to slot rect 45x42
    # centered; use drawArc equivalent via ImageDraw.arc
    # We'll draw dot + 3 arcs
    # determine s as min(w,h) ~ 32? but slot is 45x42; we use icon_size 22 in HomeRef but slot larger
    # Place glyph centered
    s = min(w, h) - 8
    if s < 10:
        s = min(w, h)
    cx = x + w // 2
    cy = y + h - 4
    # dot 2x2
    draw.rectangle([cx - 1, cy - 1, cx + 1, cy + 1], fill=0)
    radii = [5, 10, 15]
    # Adjust to slot size: scale radii relative to s
    # Use original radii for 22px icon; scale factor s/22
    scale = s / 22.0 if s else 1.0
    for r in radii:
        rr = max(1, int(r * scale))
        # draw two quarter arcs left and right (upper half)
        # bbox for arc centered at cx,cy radius rr
        bbox = [cx - rr, cy - rr, cx + rr, cy + rr]
        # Pillow arc angles: 0 at east, counterclockwise; we need top arcs 180..360
        # Draw wide arc for wifi: we want upper half only. Original draws drawArc with dir -1/-1 and 1/-1
        # That's left upper and right upper quarters.
        # Approximate by drawing full upper half 180-360 with width 1.
        try:
            draw.arc(bbox, start=180, end=360, fill=0, width=1)
        except TypeError:
            draw.arc(bbox, start=180, end=360, fill=0)
        # For left/right split original draws two arcs but full half is okay visually.
    # If still not visible, add small extra detail
    pass


def _draw_line_icon_folder(draw, x: int, y: int, s: int):
    # mimic drawLineIconFolder: folder with tab
    pad = max(2, s // 8)
    bodyW = s - pad * 2
    tabH = max(5, s // 7)
    bodyH = s - pad - tabH - 2
    bx = x + pad
    by = y + pad + tabH - 1
    draw.rectangle([bx, y + pad, bx + bodyW // 3 + 2, y + pad + tabH], outline=0, width=1)
    draw.rectangle([bx, by, bx + bodyW, by + bodyH], outline=0, width=1)


def _draw_line_icon_weread(draw, x: int, y: int, s: int):
    bw = s // 2 + 4
    bh = s // 2 - 2
    r = max(5, s // 7)
    try:
        draw.rounded_rectangle([x + 2, y + 2, x + 2 + bw, y + 2 + bh], radius=r, outline=0, width=1)
        draw.rounded_rectangle([x + s - bw - 2, y + s - bh - 2, x + s - 2, y + s - 2], radius=r, outline=0, width=1)
    except Exception:
        draw.rectangle([x + 2, y + 2, x + 2 + bw, y + 2 + bh], outline=0, width=1)
        draw.rectangle([x + s - bw - 2, y + s - bh - 2, x + s - 2, y + s - 2], outline=0, width=1)


def _draw_line_icon_tomato(draw, x: int, y: int, s: int):
    cx = x + s // 2
    cy = y + s // 2 + 3
    r = s // 2 - 6
    if r < 2:
        draw.rectangle([cx - 1, cy - 1, cx + 1, cy + 1], fill=0)
        return
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=0, width=1)
    draw.line([cx, y + 2, cx, cy - r], fill=0, width=1)
    draw.line([cx, y + 5, cx - r // 2 - 2, y + 2], fill=0, width=1)
    draw.line([cx, y + 5, cx + r // 2 + 2, y + 2], fill=0, width=1)


def _draw_line_icon_jinjiang(draw, x: int, y: int, s: int):
    # two J hooks
    def stemJ(jx, top, bot, hook):
        draw.rectangle([jx - 1, top, jx + 1, top + 3], fill=0)
        draw.line([jx, top + 6, jx, bot], fill=0, width=1)
        draw.line([jx + 1, top + 6, jx + 1, bot], fill=0, width=1)
        draw.line([jx, bot, jx - hook, bot], fill=0, width=1)
        draw.line([jx - hook, bot, jx - hook, bot - s // 8], fill=0, width=1)
    left = x + s // 2 - s // 6
    right = x + s // 2 + s // 6
    stemJ(left, y + 4, y + s - 6, s // 6)
    stemJ(right, y + 2, y + s - 4, s // 6)


def _apply_cover_corners(image, slots):
    # image is Pillow Image 'L' with draw already; we will erase corners where radius>0
    # use pixel-level erasure as spec: procedural white spans
    import math
    pix = image.load()
    w_img, h_img = image.size
    for slot in slots:
        if slot.stype != 2:  # cover
            continue
        r = slot.radius
        if r <= 0:
            continue
        x, y, w, h = slot.x, slot.y, slot.w, slot.h
        # For each corner, iterate r x r square
        # Top-left
        cx_tl = x + r
        cy_tl = y + r
        cx_tr = x + w - r
        cy_tr = y + r
        cx_bl = x + r
        cy_bl = y + h - r
        cx_br = x + w - r
        cy_br = y + h - r
        # helper to erase outside quarter circle
        for dy in range(r):
            for dx in range(r):
                # pixel center
                # TL
                px = x + dx
                py = y + dy
                if 0 <= px < w_img and 0 <= py < h_img:
                    # distance from center of rounded corner's interior center?
                    # Use integer center at (x+r, y+r) with 0.5 offset for pixel center
                    d = math.hypot((px + 0.5) - cx_tl, (py + 0.5) - cy_tl)
                    if d > r - 0.5:
                        pix[px, py] = 255
                # TR
                px = x + w - 1 - dx
                py = y + dy
                if 0 <= px < w_img and 0 <= py < h_img:
                    d = math.hypot((px + 0.5) - cx_tr, (py + 0.5) - cy_tr)
                    if d > r - 0.5:
                        pix[px, py] = 255
                # BL
                px = x + dx
                py = y + h - 1 - dy
                if 0 <= px < w_img and 0 <= py < h_img:
                    d = math.hypot((px + 0.5) - cx_bl, (py + 0.5) - cy_bl)
                    if d > r - 0.5:
                        pix[px, py] = 255
                # BR
                px = x + w - 1 - dx
                py = y + h - 1 - dy
                if 0 <= px < w_img and 0 <= py < h_img:
                    d = math.hypot((px + 0.5) - cx_br, (py + 0.5) - cy_br)
                    if d > r - 0.5:
                        pix[px, py] = 255
    # After erasing, redraw 1px rounded borders for each cover
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    for slot in slots:
        if slot.stype != 2:
            continue
        r = slot.radius
        x, y, w, h = slot.x, slot.y, slot.w, slot.h
        try:
            # Pillow rounded_rectangle with outline
            draw.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, outline=0, width=1)
        except Exception:
            draw.rectangle([x, y, x + w - 1, y + h - 1], outline=0, width=1)


def _draw_focus(image, slots, focus_order: int | None):
    if focus_order is None:
        return
    # find slot with matching focus_order and focusable
    target = None
    for s in slots:
        if s.focusable and s.focus_order == focus_order:
            target = s
            break
    if target is None:
        return
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    inset = 3  # HomeRef FocusInset
    x, y, w, h = target.x, target.y, target.w, target.h
    r = target.radius
    # focus rect expands outward by inset
    fx = x - inset
    fy = y - inset
    fw = w + inset * 2
    fh = h + inset * 2
    fr = r + inset
    # clamp to screen
    fx = max(0, fx)
    fy = max(0, fy)
    if fx + fw > SCREEN_W:
        fw = SCREEN_W - fx
    if fy + fh > SCREEN_H:
        fh = SCREEN_H - fy
    # draw focus as 2px rounded outline (darker)
    try:
        # outer 2px black rounded rect
        draw.rounded_rectangle([fx, fy, fx + fw - 1, fy + fh - 1], radius=fr, outline=0, width=2)
        # inner white 1px inset to create contrast for e-ink (optional)
        # draw inner inset white line for focus visibility on black background? skip
    except Exception:
        draw.rectangle([fx, fy, fx + fw - 1, fy + fh - 1], outline=0, width=2)


def _scene_sample_data():
    # Sample host data for preview only - never package/runtime defaults
    current = {
        "exists": True,
        "title": "长风渡",
        "author": "墨书白 著",
        "source": "晋江文学城 · 完结",
        "cover": "长",
        "progress": 68,
    }
    recent = [
        {"id": "r1", "title": "长安第一美人", "author": "发发", "cover": "长", "progress": 20},
        {"id": "r2", "title": "春日越轨", "author": "作者2", "cover": "春", "progress": 45},
        {"id": "r3", "title": "我在古代开书局", "author": "作者3", "cover": "我", "progress": 10},
    ]
    apps = [
        {"id": "files", "name": "文件", "icon": "folder", "title": "文件"},
        {"id": "weread", "name": "微信读书", "icon": "weread", "title": "微信读书"},
        {"id": "fanqie", "name": "番茄", "icon": "tomato", "title": "番茄"},
        {"id": "jinjiang", "name": "晋江", "icon": "jinjiang", "title": "晋江"},
    ]
    system = {"battery": 78, "wifi_connected": True}
    return {"current": current, "recent": recent, "apps": apps, "system": system}

def _resolve_scene_binding(binding_str: str, sample):
    # binding_str like "$current.title" or "$item.title"
    # sample is dict with current/recent/apps/system, plus optional item context
    # For preview we resolve to string/int; missing -> None (fail safe)
    try:
        if binding_str == "$system.battery":
            return f"{sample['system']['battery']}%"  # e.g. "78%"
        if binding_str == "$system.wifi.connected":
            return sample["system"]["wifi_connected"]
        if binding_str.startswith("$current."):
            key = binding_str.split(".",1)[1]
            return sample["current"].get(key)
        if binding_str in ("$recent", "$apps"):
            return sample.get(binding_str[1:])  # list
        if binding_str.startswith("$item."):
            # item context must be provided via sample['item']
            item = sample.get("item")
            if item is None:
                return None
            key = binding_str.split(".",1)[1]
            # map name/title etc
            if key == "name":
                return item.get("name") or item.get("title")
            return item.get(key)
    except Exception:
        return None
    return None

def _render_scene_commands(image, draw, ImageFont, regular_path, bold_path, scene_raw, theme_id, verbose=False):
    # Decode scene raw via compile_home_theme decode helpers for full fidelity
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location("cht_scene_render", str(ROOT / "firmware" / "tools" / "compile_home_theme.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        # we will not use high-level decode_scene list only; we need to walk binary in order preserving padding
        # Use manual walk to preserve exact order and handle repeats
        data = scene_raw
        ver, cnt, flags, res = struct.unpack("<HHHH", data[:8])
        off = 8
        sample = _scene_sample_data()
        # helper to get font for text
        def font_for_id(fid, style):
            px = FONT_ID_TO_PX.get(fid, FALLBACK_PX)
            bold = (style == 1)
            path = bold_path if bold and bold_path else regular_path
            if path is None:
                return ImageFont.load_default()
            try:
                return ImageFont.truetype(str(path), px)
            except:
                return ImageFont.load_default()
        # walk commands in exact order
        for idx in range(cnt):
            if off+4 > len(data):
                break
            ntype, cflags, plen = struct.unpack("<BBH", data[off:off+4])
            off +=4
            payload = data[off:off+plen]
            # padding skip after payload will be done at end
            # parse common prefix: visible_if, action
            p_off = 0
            visible_binding = None
            action_id = None
            action_arg = None
            if cflags & 0x01:
                if p_off < len(payload):
                    vb = payload[p_off]
                    p_off+=1
                    # map back to binding string
                    rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                    visible_binding = rev.get(vb)
            if cflags & 0x02:
                if p_off < len(payload):
                    action_id = payload[p_off]
                    p_off+=1
                    if p_off < len(payload):
                        has_arg = payload[p_off]
                        p_off+=1
                        if has_arg and p_off < len(payload):
                            ab = payload[p_off]
                            p_off+=1
                            rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                            action_arg = rev.get(ab)
            # visible_if check
            if visible_binding:
                val = _resolve_scene_binding(visible_binding, sample)
                if not val:
                    # skip this node entirely (fail safe hidden)
                    off += plen
                    off = (off+3)//4*4
                    continue
            # type-specific draw
            if ntype == mod.SCENE_NODE_CLEAR:
                if p_off < len(payload):
                    col = payload[p_off]
                    p_off+=1
                    fill = 255 if col==0 else 0
                    draw.rectangle([0,0, SCREEN_W-1, SCREEN_H-1], fill=fill)
            elif ntype == mod.SCENE_NODE_BITMAP:
                # bitmap: x,y,w,h + source string
                if p_off+8 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    # source string
                    if p_off+2 <= len(payload):
                        slen = struct.unpack("<H", payload[p_off:p_off+2])[0]
                        p_off+=2
                        src = payload[p_off:p_off+slen].decode("utf-8", errors="replace") if slen>0 else ""
                        # For preview, draw bitmap placeholder as light gray or checkboard
                        # If we have real file, we could load but sample data not needed; just draw rect
                        draw.rectangle([x,y,x+w-1,y+h-1], fill=180, outline=0)
                        # mark source text
                        try:
                            f = ImageFont.load_default()
                            draw.text((x+2,y+2), f"bmp:{src[:10]}", fill=0, font=f)
                        except:
                            pass
                    else:
                        draw.rectangle([x,y,x+w-1,y+h-1], fill=180, outline=0)
                else:
                    pass
            elif ntype == mod.SCENE_NODE_LINE:
                if p_off+10 <= len(payload):
                    x,y,x2,y2 = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    wid = payload[p_off]
                    p_off+=1
                    col = payload[p_off]
                    p_off+=1
                    fill = 255 if col==0 else 0
                    # draw line as rectangle for width
                    if y==y2:
                        # horizontal
                        draw.rectangle([min(x,x2), y, max(x,x2), y+wid-1], fill=fill)
                    elif x==x2:
                        draw.rectangle([x, min(y,y2), x+wid-1, max(y,y2)], fill=fill)
                    else:
                        draw.line([x,y,x2,y2], fill=fill, width=wid)
            elif ntype == mod.SCENE_NODE_RECT:
                if p_off+10 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    stroke = payload[p_off]
                    p_off+=1
                    fill = payload[p_off]
                    p_off+=1
                    if fill:
                        draw.rectangle([x,y,x+w-1,y+h-1], fill=0 if stroke else 255, outline=0 if stroke==0 else 0, width=stroke if stroke else 1)
                    else:
                        if stroke>0:
                            draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=stroke)
            elif ntype == mod.SCENE_NODE_ROUND_RECT:
                if p_off+11 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    r = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    stroke = payload[p_off]
                    p_off+=1
                    fill = payload[p_off] if p_off < len(payload) else 0
                    p_off+=1
                    try:
                        if fill:
                            draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, fill=255, outline=0, width=stroke if stroke else 1)
                        else:
                            if stroke>0:
                                draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, outline=0, width=stroke)
                            else:
                                draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, outline=0)
                    except:
                        draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=stroke if stroke else 1)
            elif ntype == mod.SCENE_NODE_TEXT:
                if p_off+8+4+2+2 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    fid,fstyle,align,ellipsis = struct.unpack("<BBBB", payload[p_off:p_off+4])
                    p_off+=4
                    is_binding, bid = struct.unpack("<BB", payload[p_off:p_off+2])
                    p_off+=2
                    tlen = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    text_val = ""
                    if is_binding:
                        rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                        bstr = rev.get(bid)
                        if bstr:
                            # resolve sample
                            # handle progress special: format maybe with % ?
                            val = _resolve_scene_binding(bstr, sample)
                            if val is None:
                                text_val = ""  # fail safe empty
                            else:
                                if bstr == "$current.progress":
                                    # sample is int, format as "68%"
                                    text_val = f"{val}%"
                                else:
                                    text_val = str(val)
                        else:
                            text_val = ""
                    else:
                        if tlen>0:
                            text_val = payload[p_off:p_off+tlen].decode("utf-8", errors="replace")
                            p_off+=tlen
                        else:
                            text_val = ""
                    if not text_val:
                        # skip empty
                        pass
                    else:
                        font = font_for_id(fid, fstyle)
                        # ellipsis handling
                        max_w = w
                        fitted = _fit_text(draw, text_val, font, max_w-2 if max_w>2 else max_w)
                        tw = _text_width(draw, fitted, font)
                        if align==1:
                            tx = x + (w - tw)//2
                        elif align==2:
                            tx = x + w - tw -1
                        else:
                            tx = x+2
                        try:
                            bbox = draw.textbbox((0,0), fitted, font=font)
                            th = bbox[3]-bbox[1]
                        except:
                            th = h
                        ty = y + (h - th)//2
                        ty = max(y, min(ty, y+h-th))
                        draw.text((tx,ty), fitted, font=font, fill=0)
                else:
                    pass
            elif ntype == mod.SCENE_NODE_COVER:
                if p_off+10 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    r = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    has_bind = payload[p_off] if p_off < len(payload) else 0
                    p_off+=1
                    bstr = None
                    if has_bind:
                        bid = payload[p_off]
                        p_off+=1
                        rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                        bstr = rev.get(bid)
                    # resolve cover glyph: use sample
                    glyph = "?"
                    if bstr:
                        val = _resolve_scene_binding(bstr, sample)
                        if isinstance(val, str) and val:
                            glyph = val[0]
                        elif bstr == "$item.cover":
                            # need item context? For cover outside repeat, it's current cover
                            # We are in non-repeat context, so use current cover
                            glyph = sample["current"]["cover"][0] if sample["current"]["cover"] else "?"
                        else:
                            glyph = "C"
                    else:
                        glyph = "C"
                    # draw cover placeholder
                    draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=None)
                    # glyph
                    glyph_px = min(w,h)//2
                    glyph_px = max(12, min(glyph_px, 48))
                    try:
                        path = bold_path or regular_path
                        gf = ImageFont.truetype(str(path), glyph_px) if path else ImageFont.load_default()
                    except:
                        gf = ImageFont.load_default()
                    try:
                        bbox = draw.textbbox((0,0), glyph, font=gf)
                        gw = bbox[2]-bbox[0]; gh = bbox[3]-bbox[1]
                    except:
                        gw = gh = glyph_px
                    gx = x + (w - gw)//2
                    gy = y + (h - gh)//2 -2
                    try:
                        draw.text((gx,gy), glyph, font=gf, fill=0)
                    except:
                        draw.text((gx,gy), glyph, fill=0)
                    # border
                    try:
                        draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, outline=0, width=1)
                    except:
                        draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
                else:
                    pass
            elif ntype == mod.SCENE_NODE_PROGRESS:
                if p_off+11 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    r = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    bid = payload[p_off] if p_off < len(payload) else 0
                    p_off+=1
                    rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                    bstr = rev.get(bid, "$current.progress")
                    val = _resolve_scene_binding(bstr, sample) if bstr else 0
                    try:
                        pct = int(str(val).replace("%",""))
                    except:
                        pct = 38
                    pct = max(0,min(100,pct))
                    if r<=0:
                        r = h//2
                    try:
                        draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, fill=255, outline=0, width=1)
                    except:
                        draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=0)
                        draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
                    if pct>0 and w>4 and h>4:
                        inner_w = w-4
                        fill_w = max(h-4, (inner_w * pct)//100)
                        fill_w = min(inner_w, fill_w)
                        try:
                            draw.rounded_rectangle([x+2,y+2,x+2+fill_w-1,y+h-2-1], radius=(h-4)//2, fill=0)
                        except:
                            draw.rectangle([x+2,y+2,x+2+fill_w-1,y+h-2-1], fill=0)
                else:
                    pass
            elif ntype == mod.SCENE_NODE_ICON:
                if p_off+8 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    if p_off+2 <= len(payload):
                        slen = struct.unpack("<H", payload[p_off:p_off+2])[0]
                        p_off+=2
                        name = payload[p_off:p_off+slen].decode("utf-8", errors="replace") if slen>0 else ""
                        p_off+=slen
                        has_bind = payload[p_off] if p_off < len(payload) else 0
                        p_off+=1
                        bstr=None
                        if has_bind and p_off < len(payload):
                            bid = payload[p_off]
                            p_off+=1
                            rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                            bstr = rev.get(bid)
                        # resolve icon: sample item icon
                        # For preview, draw placeholder icon shape based on name
                        # Use simple rectangle with label
                        draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=0, width=1)
                        # draw icon glyph: first letter of name or bound item name
                        icon_char = "I"
                        if bstr:
                            # need item context: if we are inside repeat, icon binding is $item.icon, sample item will be provided via outer repeat handling
                            # But for top-level icon (not in repeat) we resolve via sample directly (no item)
                            # For repeat we handle separately, so this branch is top-level
                            val = _resolve_scene_binding(bstr, sample)
                            if val:
                                icon_char = str(val)[0]
                        else:
                            if name:
                                icon_char = name[0]
                        # draw centered char
                        try:
                            path = regular_path
                            f = ImageFont.truetype(str(path), min(w,h)//2) if path else ImageFont.load_default()
                        except:
                            f = ImageFont.load_default()
                        try:
                            bbox = draw.textbbox((0,0), icon_char, font=f)
                            gw = bbox[2]-bbox[0]; gh = bbox[3]-bbox[1]
                        except:
                            gw=gh=10
                        gx = x + (w - gw)//2
                        gy = y + (h - gh)//2
                        draw.text((gx,gy), icon_char, font=f, fill=0)
                        # border
                        try:
                            draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
                        except:
                            pass
                    else:
                        pass
            elif ntype == mod.SCENE_NODE_BATTERY:
                if p_off+9 <= len(payload):
                    x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8])
                    p_off+=8
                    bid = payload[p_off] if p_off < len(payload) else 0
                    rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                    bstr = rev.get(bid, "$system.battery")
                    val = _resolve_scene_binding(bstr, sample) if bstr else "78%"
                    # val like "78%"
                    try:
                        pct = int(str(val).replace("%",""))
                    except:
                        pct = 78
                    # draw battery outline
                    try:
                        draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=2, outline=0, width=1, fill=255)
                    except:
                        draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1, fill=255)
                    # fill proportional
                    fill_w = max(2, (w-4)*pct//100)
                    draw.rectangle([x+2,y+2,x+2+fill_w-1,y+h-2-1], fill=0)
                    # nub
                    draw.rectangle([x+w-1, y+h//3, x+w+3, y+ h*2//3], fill=0)
                else:
                    pass
            elif ntype == mod.SCENE_NODE_GROUP:
                if p_off+2 <= len(payload):
                    child_cnt = struct.unpack("<H", payload[p_off:p_off+2])[0]
                    p_off+=2
                    # children are encoded immediately after payload prefix as concatenated commands
                    # Need to recursively render children in order
                    # children bytes are payload[p_off:]
                    child_data = payload[p_off:]
                    # walk children same as top-level but with depth
                    c_off = 0
                    for ci in range(child_cnt):
                        if c_off+4 > len(child_data):
                            break
                        ct, cf, cplen = struct.unpack("<BBH", child_data[c_off:c_off+4])
                        c_off+=4
                        # For group child rendering we could recursively call rendering by constructing a mini scene
                        # Simpler: decode child and draw directly by re-invoking same logic via temporary image walk
                        # We'll manually handle by creating a sub-payload and re-entering same branch via recursion helper
                        # Instead of duplicating logic, we create a helper function call
                        # To avoid recursion complexity, we inline a call to a helper that draws single node
                        # For now, handle only simple children types: cover/text etc by decoding one node at a time
                        # We'll extract child's payload and render using same code path by recursing via function
                        cpayload = child_data[c_off:c_off+cplen]
                        # temporarily craft a single node rendering by calling same switch
                        # Use a helper lambda
                        # To reuse code, we can call a small function _render_single_node
                        # For simplicity, we will just decode and draw using same logic but with offset handling
                        # Since group children are absolute rects, we can render them immediately
                        # Create a temporary buffer for single node: header + payload
                        # We'll reuse the same ntype handling by directly invoking drawing for that child
                        # Instead of duplicating, we call _render_scene_single
                        _render_scene_single(image, draw, ImageFont, regular_path, bold_path, ct, cf, cpayload, sample)
                        c_off += cplen
                        # align
                        c_off = (c_off+3)//4*4
            elif ntype == mod.SCENE_NODE_REPEAT:
                # Repeat: parse header
                if p_off+12 <= len(payload):
                    sid, lim, rx, ry, iw, ih, gap = struct.unpack("<BBHHHHH", payload[p_off:p_off+12])
                    p_off+=12
                    dir_val, pad, child_cnt = struct.unpack("<BBH", payload[p_off:p_off+4])
                    p_off+=4
                    child_data = payload[p_off:]
                    # Determine source list
                    rev = {v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                    src_str = rev.get(sid)
                    src_list = []
                    if src_str == "$recent":
                        src_list = sample["recent"][:lim]
                    elif src_str == "$apps":
                        src_list = sample["apps"][:lim]
                    else:
                        src_list = []
                    # For each item, render children with offset
                    c_off_dummy = 0
                    # Pre-parse children templates list
                    templates = []
                    t_off = 0
                    for ci in range(child_cnt):
                        if t_off+4 > len(child_data):
                            break
                        ct, cf, cplen = struct.unpack("<BBH", child_data[t_off:t_off+4])
                        t_off+=4
                        cpayload = child_data[t_off:t_off+cplen]
                        templates.append((ct, cf, cpayload))
                        t_off += cplen
                        t_off = (t_off+3)//4*4
                    # Now render each item
                    for idx, item in enumerate(src_list):
                        # compute item origin
                        if dir_val == 0: # horizontal
                            ox = rx + idx * (iw + gap)
                            oy = ry
                        else:
                            ox = rx
                            oy = ry + idx * (ih + gap)
                        # set item context
                        sample_with_item = dict(sample)
                        sample_with_item["item"] = item
                        for (ct, cf, cpayload) in templates:
                            # need to offset rects inside child by ox,oy
                            # For child types that have rect, we add offset
                            # We'll create a helper that draws with offset
                            _render_scene_single_offset(image, draw, ImageFont, regular_path, bold_path, ct, cf, cpayload, sample_with_item, ox, oy)
                else:
                    pass
            else:
                # unknown type skip
                pass
            # move to next command (skip padding)
            off += plen
            off = (off+3)//4*4
        # end for
    except Exception as e:
        import traceback
        print(f"scene render failed: {e}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)

# helpers for group/repeat single node rendering (defined later but need forward)
def _render_scene_single(image, draw, ImageFont, regular_path, bold_path, ntype, cflags, payload, sample):
    _render_scene_single_offset(image, draw, ImageFont, regular_path, bold_path, ntype, cflags, payload, sample, 0, 0)

def _render_scene_single_offset(image, draw, ImageFont, regular_path, bold_path, ntype, cflags, payload, sample, ox, oy):
    # Reuse same drawing logic but with offset ox,oy added to child's rect
    # This is a lightweight duplicative handler for repeat/group children
    # We will import mod again for constants
    import importlib.util
    spec = importlib.util.spec_from_file_location("cht_single", str(ROOT / "firmware" / "tools" / "compile_home_theme.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    p_off = 0
    visible_binding = None
    action_id=None
    if cflags & 0x01:
        if p_off < len(payload):
            vb = payload[p_off]; p_off+=1
            rev={v:k for k,v in mod.BINDING_SCENE_MAP.items()}
            visible_binding=rev.get(vb)
    if cflags & 0x02:
        if p_off < len(payload):
            action_id=payload[p_off]; p_off+=1
            if p_off < len(payload):
                has_arg=payload[p_off]; p_off+=1
                if has_arg and p_off < len(payload):
                    p_off+=1
    if visible_binding:
        val=_resolve_scene_binding(visible_binding, sample)
        if not val:
            return
    def font_for_id(fid, style):
        px = FONT_ID_TO_PX.get(fid, FALLBACK_PX)
        bold=(style==1)
        path = bold_path if bold and bold_path else regular_path
        if path is None:
            return ImageFont.load_default()
        try:
            return ImageFont.truetype(str(path), px)
        except:
            return ImageFont.load_default()
    if ntype == mod.SCENE_NODE_COVER:
        if p_off+10 <= len(payload):
            x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8]); p_off+=8
            r=struct.unpack("<H", payload[p_off:p_off+2])[0]; p_off+=2
            has_bind=payload[p_off] if p_off < len(payload) else 0; p_off+=1
            bstr=None
            if has_bind:
                bid=payload[p_off]; p_off+=1
                rev={v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                bstr=rev.get(bid)
            x+=ox; y+=oy
            glyph="?"
            if bstr:
                val=_resolve_scene_binding(bstr, sample)
                if isinstance(val,str) and val:
                    glyph=val[0]
                else:
                    glyph="C"
            draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=None)
            glyph_px=min(w,h)//2; glyph_px=max(12, min(glyph_px,48))
            try:
                path=bold_path or regular_path
                gf=ImageFont.truetype(str(path), glyph_px) if path else ImageFont.load_default()
            except:
                gf=ImageFont.load_default()
            try:
                bbox=draw.textbbox((0,0), glyph, font=gf); gw=bbox[2]-bbox[0]; gh=bbox[3]-bbox[1]
            except:
                gw=gh=glyph_px
            gx=x + (w - gw)//2; gy=y + (h - gh)//2 -2
            try:
                draw.text((gx,gy), glyph, font=gf, fill=0)
            except:
                draw.text((gx,gy), glyph, fill=0)
            try:
                draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, outline=0, width=1)
            except:
                draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
    elif ntype == mod.SCENE_NODE_TEXT:
        if p_off+8+4+2+2 <= len(payload):
            x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8]); p_off+=8
            x+=ox; y+=oy
            fid,fstyle,align,ellipsis = struct.unpack("<BBBB", payload[p_off:p_off+4]); p_off+=4
            is_binding,bid = struct.unpack("<BB", payload[p_off:p_off+2]); p_off+=2
            tlen=struct.unpack("<H", payload[p_off:p_off+2])[0]; p_off+=2
            text_val=""
            if is_binding:
                rev={v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                bstr=rev.get(bid)
                if bstr:
                    val=_resolve_scene_binding(bstr, sample)
                    if val is None:
                        text_val=""
                    else:
                        if bstr=="$current.progress":
                            text_val=f"{val}%"
                        else:
                            text_val=str(val)
                else:
                    text_val=""
            else:
                if tlen>0:
                    text_val=payload[p_off:p_off+tlen].decode("utf-8", errors="replace")
                    p_off+=tlen
            if not text_val:
                return
            font=font_for_id(fid,fstyle)
            fitted=_fit_text(draw, text_val, font, w-2 if w>2 else w)
            tw=_text_width(draw, fitted, font)
            if align==1:
                tx=x + (w - tw)//2
            elif align==2:
                tx=x + w - tw -1
            else:
                tx=x+2
            try:
                bbox=draw.textbbox((0,0), fitted, font=font); th=bbox[3]-bbox[1]
            except:
                th=h
            ty=y + (h - th)//2; ty=max(y, min(ty, y+h-th))
            draw.text((tx,ty), fitted, font=font, fill=0)
    elif ntype == mod.SCENE_NODE_ICON:
        if p_off+8 <= len(payload):
            x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8]); p_off+=8
            x+=ox; y+=oy
            if p_off+2 <= len(payload):
                slen=struct.unpack("<H", payload[p_off:p_off+2])[0]; p_off+=2
                name=payload[p_off:p_off+slen].decode("utf-8", errors="replace") if slen>0 else ""; p_off+=slen
                has_bind=payload[p_off] if p_off < len(payload) else 0; p_off+=1
                bstr=None
                if has_bind and p_off < len(payload):
                    bid=payload[p_off]; p_off+=1
                    rev={v:k for k,v in mod.BINDING_SCENE_MAP.items()}
                    bstr=rev.get(bid)
                icon_char="I"
                if bstr:
                    val=_resolve_scene_binding(bstr, sample)
                    if val:
                        icon_char=str(val)[0]
                else:
                    if name:
                        icon_char=name[0]
                draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=0, width=1)
                try:
                    path=regular_path
                    f=ImageFont.truetype(str(path), min(w,h)//2) if path else ImageFont.load_default()
                except:
                    f=ImageFont.load_default()
                try:
                    bbox=draw.textbbox((0,0), icon_char, font=f); gw=bbox[2]-bbox[0]; gh=bbox[3]-bbox[1]
                except:
                    gw=gh=10
                gx=x + (w - gw)//2; gy=y + (h - gh)//2
                draw.text((gx,gy), icon_char, font=f, fill=0)
                try:
                    draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
                except:
                    pass
    elif ntype == mod.SCENE_NODE_PROGRESS:
        if p_off+11 <= len(payload):
            x,y,w,h = struct.unpack("<HHHH", payload[p_off:p_off+8]); p_off+=8
            x+=ox; y+=oy
            r=struct.unpack("<H", payload[p_off:p_off+2])[0]; p_off+=2
            bid=payload[p_off] if p_off < len(payload) else 0
            rev={v:k for k,v in mod.BINDING_SCENE_MAP.items()}
            bstr=rev.get(bid, "$current.progress")
            val=_resolve_scene_binding(bstr, sample) if bstr else 0
            try:
                pct=int(str(val).replace("%",""))
            except:
                pct=38
            pct=max(0,min(100,pct))
            if r<=0:
                r=h//2
            try:
                draw.rounded_rectangle([x,y,x+w-1,y+h-1], radius=r, fill=255, outline=0, width=1)
            except:
                draw.rectangle([x,y,x+w-1,y+h-1], fill=255, outline=0)
                draw.rectangle([x,y,x+w-1,y+h-1], outline=0, width=1)
            if pct>0 and w>4 and h>4:
                inner_w=w-4
                fill_w=max(h-4, (inner_w * pct)//100)
                fill_w=min(inner_w, fill_w)
                try:
                    draw.rounded_rectangle([x+2,y+2,x+2+fill_w-1,y+h-2-1], radius=(h-4)//2, fill=0)
                except:
                    draw.rectangle([x+2,y+2,x+2+fill_w-1,y+h-2-1], fill=0)
    else:
        # For other types in group, fallback to no-op or simple rect
        pass

def render_preview(m4theme_path: Path, out_path: Path, focus_order: int | None = 0,
                   font_path: Path | None = None, bold_font_path: Path | None = None,
                   verbose: bool = True):
    from PIL import Image, ImageDraw, ImageFont

    decoded = decode_m4theme(m4theme_path)
    slots = decoded["slots"]
    bg = decoded["background"]
    theme_id = decoded["theme_id"]
    scene_raw = decoded.get("scene_raw")
    scene = decoded.get("scene")

    regular_path, bold_path, font_note = resolve_font_paths(font_path, bold_font_path)
    if verbose:
        print(font_note, file=sys.stderr)

    # Create white canvas 480x800
    image = Image.new("L", (SCREEN_W, SCREEN_H), 255)
    draw = ImageDraw.Draw(image)

    # If SCENE section present, render compiled command stream in exact order (new Home Scene V1 path)
    # This preserves JSON nodes order = draw order, and handles bitmap before/after cover ordering.
    # Sample values are preview-only.
    if scene_raw is not None:
        _render_scene_commands(image, draw, ImageFont, regular_path, bold_path, scene_raw, theme_id, verbose=verbose)
        # For scene packs, legacy background is white fallback; overlay is no-op but we still apply
        # legacy template overlay after scene to keep legacy chrome if any (for hybrid packs)
        try:
            pix = image.load()
            for y in range(SCREEN_H):
                row_off = y * STRIDE
                for byte_idx in range(STRIDE):
                    b = bg[row_off + byte_idx]
                    if b == 0:
                        continue
                    base_x = byte_idx * 8
                    if b & 0x80:
                        pix[base_x, y] = 0
                    if b & 0x40 and base_x + 1 < SCREEN_W:
                        pix[base_x + 1, y] = 0
                    if b & 0x20 and base_x + 2 < SCREEN_W:
                        pix[base_x + 2, y] = 0
                    if b & 0x10 and base_x + 3 < SCREEN_W:
                        pix[base_x + 3, y] = 0
                    if b & 0x08 and base_x + 4 < SCREEN_W:
                        pix[base_x + 4, y] = 0
                    if b & 0x04 and base_x + 5 < SCREEN_W:
                        pix[base_x + 5, y] = 0
                    if b & 0x02 and base_x + 6 < SCREEN_W:
                        pix[base_x + 6, y] = 0
                    if b & 0x01 and base_x + 7 < SCREEN_W:
                        pix[base_x + 7, y] = 0
        except Exception as e:
            raise RuntimeError(f"background overlay failed: {e}")
        # Save
        out_path = Path(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        image.save(out_path, "PNG")
        if verbose:
            print(f"preview: theme={theme_id} scene={len(scene) if scene else 0} bg={len(bg)} -> {out_path} ({SCREEN_W}x{SCREEN_H})", file=sys.stderr)
            print(f"decoded: theme_id={theme_id} from {m4theme_path}", file=sys.stderr)
        return out_path

    # Helper to cache fonts per slot
    font_cache = {}

    def get_font(slot):
        if slot.index not in font_cache:
            font_cache[slot.index] = _font_for_slot(ImageFont, regular_path, bold_path, slot)
        return font_cache[slot.index]

    # 2. Dynamic content (covers, text, progress, wifi, quick entries, bottom)
    # For template Home (prebinarized_alpha), covers are rectangular underneath;
    # the black-ink template overlay restores rounded frames last (never paint white over dynamic).
    # First pass: covers as rectangular placeholders
    # We need mapping from cover index order to glyph; use slot order for cover binding
    cover_indices = [s.index for s in slots if s.stype == 2]
    # Map cover slot index to ordinal for glyph selection
    cover_ordinal = {sid: i for i, sid in enumerate(sorted(cover_indices))}
    # For mini covers, use their own glyph
    for slot in slots:
        if slot.stype == 2:  # cover
            x, y, w, h = slot.x, slot.y, slot.w, slot.h
            # Fill white rectangular cover area (simulates bitmap)
            draw.rectangle([x, y, x + w - 1, y + h - 1], fill=255, outline=None)
            # Draw subtle cover pattern: a light gray diagonal or placeholder glyph
            # For 1-bit realism, we draw a thin inner rectangle to suggest content? Use white base then black glyph.
            # Determine glyph from ordinal
            ord_idx = cover_ordinal.get(slot.index, 0)
            glyph = COVER_GLYPH.get(ord_idx, "?")
            # choose glyph size ~ 1/3 of shorter side
            glyph_px = min(w, h) // 2
            # clamp
            glyph_px = max(12, min(glyph_px, 48))
            try:
                path = bold_path or regular_path
                if path:
                    gf = ImageFont.truetype(str(path), glyph_px)
                else:
                    gf = ImageFont.load_default()
            except Exception:
                gf = ImageFont.load_default()
            # center glyph
            try:
                bbox = draw.textbbox((0, 0), glyph, font=gf)
                gw = bbox[2] - bbox[0]
                gh = bbox[3] - bbox[1]
            except Exception:
                gw = glyph_px
                gh = glyph_px
            gx = x + (w - gw) // 2
            gy = y + (h - gh) // 2 - 2
            # draw glyph black
            try:
                draw.text((gx, gy), glyph, font=gf, fill=0)
            except Exception:
                draw.text((gx, gy), glyph, fill=0)
            # optional: draw small "COVER" hint at bottom for hero? not needed

    # Second pass: text slots
    for slot in slots:
        if slot.stype == 0:  # text
            # fetch sample text via binding
            txt = SAMPLE_TEXT.get(slot.binding, "")
            if not txt:
                # For missing recent bindings like recent[1].author etc not used; provide fallback
                if slot.binding in (9, 14, 19):
                    txt = "作者名"
                elif slot.binding in (10, 15, 20):
                    txt = "晋江"
                else:
                    # for bottom/quick labels? those are hitbox not text
                    continue
            font = get_font(slot)
            max_w = slot.w
            # apply fit/truncate to single line
            fitted = _fit_text(draw, txt, font, max_w - 2)
            # align handling
            align = slot.align  # 0 left,1 center,2 right
            # compute x offset within rect
            tw = _text_width(draw, fitted, font)
            if align == 1:  # center
                tx = slot.x + (slot.w - tw) // 2
            elif align == 2:  # right
                tx = slot.x + slot.w - tw - 1
            else:
                tx = slot.x + 2  # small left pad
            # vertical centering within slot.h
            try:
                bbox = draw.textbbox((0, 0), fitted, font=font)
                th = bbox[3] - bbox[1]
            except Exception:
                th = slot.h
            # HomeRef baseline adjustments approximated by centering
            # For hero_title with h=52 allows 2 lines; but we truncate to one line centered
            ty = slot.y + (slot.h - th) // 2
            # ensure not outside rect
            ty = max(slot.y, min(ty, slot.y + slot.h - th))
            # For some fonts, bbox top is negative; adjust to use anchor
            # Use simple ty + offset
            # Pillow's textbbox for Hiragino has ascent ~?
            # We'll just draw at (tx, ty)
            draw.text((tx, ty), fitted, font=font, fill=0)

    # Third pass: progress slot(s)
    for slot in slots:
        if slot.stype == 3:  # progress
            x, y, w, h = slot.x, slot.y, slot.w, slot.h
            r = slot.radius
            if r <= 0:
                r = h // 2
            # Draw outer track: white fill + black rounded border
            try:
                # outer border
                draw.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=255, outline=0, width=1)
            except Exception:
                draw.rectangle([x, y, x + w - 1, y + h - 1], fill=255, outline=0)
                draw.rectangle([x, y, x + w - 1, y + h - 1], outline=0, width=1)
            # Fill portion
            pct = SAMPLE_PROGRESS
            pct = max(0, min(100, pct))
            if pct > 0 and w > 4 and h > 4:
                inner_w = w - 4
                # ensure at least radius ensures rounded ends even for small pct
                fill_w = max(h - 4, (inner_w * pct) // 100)
                fill_w = min(inner_w, fill_w)
                try:
                    draw.rounded_rectangle([x + 2, y + 2, x + 2 + fill_w - 1, y + h - 2 - 1], radius=(h - 4) // 2, fill=0)
                except Exception:
                    draw.rectangle([x + 2, y + 2, x + 2 + fill_w - 1, y + h - 2 - 1], fill=0)
            # Optional percent text already handled via separate? In device hero progress baseline draws text separately above bar;
            # our progress slot only is bar; hero progress text is via hero_source? Actually progress slot binding is recent[0].progress but slot is bar only.
            # The progress percent text might be drawn via M4UiText separately at infoX; but theme's text slots for progress? In theme, progress is separate slot; hero title/author/source are text. Progress slot is just bar.
            # So we don't draw percent number inside bar.
            # Draw small percent label below? Not needed.
            pass

    # Fourth: icon slots (wifi_status) — skip stale wifi for mofei-classic template (not wired)
    for slot in slots:
        if slot.stype == 1:  # image/icon
            # only wifi_status uses binding 1
            if slot.binding == 1:
                if theme_id == "mofei-classic":
                    continue
                _draw_wifi_glyph(draw, slot.x, slot.y, slot.w, slot.h)
            else:
                # generic icon placeholder
                draw.rectangle([slot.x, slot.y, slot.x + slot.w - 1, slot.y + slot.h - 1], outline=0, width=1)
                draw.line([slot.x, slot.y, slot.x + slot.w - 1, slot.y + slot.h - 1], fill=0, width=1)
                draw.line([slot.x + slot.w - 1, slot.y, slot.x, slot.y + slot.h - 1], fill=0, width=1)

    # Fifth: hitbox slots -> quick entries + bottom nav (sample dynamic data)
    # Distinguish by y
    for slot in slots:
        if slot.stype == 4:  # hitbox
            # target action determines label
            action = slot.target_action if slot.target_kind == 2 else None
            # quick tiles: y around 650
            if 630 <= slot.y <= 700:
                # quick tile
                # Determine icon type by action
                x, y, w, h = slot.x, slot.y, slot.w, slot.h
                icon_s = 42  # HomeRef QuickIconSize
                icon_x = x + (w - icon_s) // 2
                icon_y = y + 8
                # draw icon per action
                if action == 0:
                    _draw_line_icon_folder(draw, icon_x, icon_y, icon_s)
                elif action == 1:
                    _draw_line_icon_weread(draw, icon_x, icon_y, icon_s)
                elif action == 2:
                    _draw_line_icon_tomato(draw, icon_x, icon_y, icon_s)
                elif action == 3:
                    _draw_line_icon_jinjiang(draw, icon_x, icon_y, icon_s)
                else:
                    draw.rectangle([icon_x, icon_y, icon_x + icon_s - 1, icon_y + icon_s - 1], outline=0, width=1)
                # label
                label = QUICK_SHORT.get(action, ACTION_LABEL.get(action, f"action{action}"))
                # pick font for quick label ~ FontQuickLabel 16
                try:
                    qp = regular_path
                    if qp:
                        qfont = ImageFont.truetype(str(qp), 13)
                    else:
                        qfont = ImageFont.load_default()
                except Exception:
                    qfont = ImageFont.load_default()
                # center label at bottom of tile
                tw = _text_width(draw, label, qfont)
                tx = x + (w - tw) // 2
                # baseline ~ QuickLabelBaseline =739 => tile y 650 + h92 => baseline 739 gives y = 739 - ascent.
                # Approximate ty as y + h - 12
                try:
                    bbox = draw.textbbox((0, 0), label, font=qfont)
                    th = bbox[3] - bbox[1]
                except Exception:
                    th = 12
                ty = y + h - th - 6
                # ensure not overflow
                fitted = _fit_text(draw, label, qfont, w - 4)
                tw = _text_width(draw, fitted, qfont)
                tx = x + (w - tw) // 2
                draw.text((tx, ty), fitted, font=qfont, fill=0)
            elif slot.y >= 760:  # bottom nav
                label = ACTION_LABEL.get(action, f"action{action}")
                # bottom nav font ~ 14
                try:
                    qp = regular_path
                    if qp:
                        bfont = ImageFont.truetype(str(qp), 14)
                    else:
                        bfont = ImageFont.load_default()
                except Exception:
                    bfont = ImageFont.load_default()
                # center in slot rect
                tw = _text_width(draw, label, bfont)
                tx = slot.x + (slot.w - tw) // 2
                try:
                    bbox = draw.textbbox((0, 0), label, font=bfont)
                    th = bbox[3] - bbox[1]
                except Exception:
                    th = 12
                ty = slot.y + (slot.h - th) // 2
                # for bottom nav, ensure y within
                fitted = _fit_text(draw, label, bfont, slot.w - 6)
                tw = _text_width(draw, fitted, bfont)
                tx = slot.x + (slot.w - tw) // 2
                draw.text((tx, ty), fitted, font=bfont, fill=0)
            else:
                # generic hitbox placeholder border (should not happen)
                draw.rectangle([slot.x, slot.y, slot.x + slot.w - 1, slot.y + slot.h - 1], outline=0, width=1)

    # 2b. Template black-ink overlay LAST (1=black, 0=transparent/no-op)
    # Correct order: A) clear white, B) dynamic covers/text/progress, C) overlay template ink, D) focus.
    # Works for both legacy threshold (white is already cleared) and prebinarized_alpha.
    try:
        pix = image.load()
        for y in range(SCREEN_H):
            row_off = y * STRIDE
            for byte_idx in range(STRIDE):
                b = bg[row_off + byte_idx]
                if b == 0:
                    continue
                base_x = byte_idx * 8
                if b & 0x80:
                    pix[base_x, y] = 0
                if b & 0x40 and base_x + 1 < SCREEN_W:
                    pix[base_x + 1, y] = 0
                if b & 0x20 and base_x + 2 < SCREEN_W:
                    pix[base_x + 2, y] = 0
                if b & 0x10 and base_x + 3 < SCREEN_W:
                    pix[base_x + 3, y] = 0
                if b & 0x08 and base_x + 4 < SCREEN_W:
                    pix[base_x + 4, y] = 0
                if b & 0x04 and base_x + 5 < SCREEN_W:
                    pix[base_x + 5, y] = 0
                if b & 0x02 and base_x + 6 < SCREEN_W:
                    pix[base_x + 6, y] = 0
                if b & 0x01 and base_x + 7 < SCREEN_W:
                    pix[base_x + 7, y] = 0
    except Exception as e:
        raise RuntimeError(f"background overlay failed: {e}")

    # 3. Cover corner masks/borders (procedural) — skip for mofei-classic template (rounded frames are in ink)
    if theme_id != "mofei-classic":
        _apply_cover_corners(image, slots)

    # 4. Focus/selection (after overlay per spec)
    _draw_focus(image, slots, focus_order)

    # Save
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Convert L to RGB for better viewer compatibility? Keep L but save as PNG gray.
    # Save as PNG
    # For e-ink accuracy, we can also threshold to pure 1-bit: image is already 0/255
    # Save with no extra filtering
    image.save(out_path, "PNG")
    if verbose:
        print(f"preview: theme={theme_id} slots={len(slots)} bg={len(bg)} focus_order={focus_order} -> {out_path} ({SCREEN_W}x{SCREEN_H})", file=sys.stderr)
        print(f"decoded: theme_id={theme_id} from {m4theme_path}", file=sys.stderr)
    return out_path


def build_parser():
    p = argparse.ArgumentParser(description="M4 Home theme host preview renderer (M4TH v1)")
    p.add_argument("--m4theme", type=Path, default=DEFAULT_M4THEME, help="path to compiled .m4theme (default build/mofei-classic.m4theme)")
    p.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output PNG path (default tmp-home-screenshots/preview-mofei-classic.png)")
    p.add_argument("--focus-order", type=int, default=0, help="focus_order to highlight (default 0 hero_cover, use -1 to disable)")
    p.add_argument("--no-focus", action="store_true", help="disable focus highlight")
    p.add_argument("--font", type=Path, help="regular TTF/TTC host font override")
    p.add_argument("--bold-font", type=Path, help="bold TTF/TTC host font override")
    p.add_argument("--quiet", action="store_true", help="suppress font note")
    return p


def main():
    parser = build_parser()
    args = parser.parse_args()
    focus = None if args.no_focus or args.focus_order < 0 else args.focus_order
    try:
        out = render_preview(args.m4theme, args.out, focus_order=focus, font_path=args.font, bold_font_path=args.bold_font, verbose=not args.quiet)
        print(out)
        return 0
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
