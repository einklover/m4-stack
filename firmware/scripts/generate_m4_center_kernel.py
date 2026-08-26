#!/usr/bin/env python3
"""Generate the 16x16 occupancy blob (CJK center-kernel + Latin/punct).

CJK U+3400–U+9FFF: absolute-position 16x16 centers + 2-bit joint class.
All other BMP cmap glyphs: 16x16 occupancy sampled like native-grid (class 1),
so the firmware embeds one blob and does not ship m4_native_grid_15x16.bin.

Occupancy source: geometry-exact run-deconvolution with slab-span-aware transition handling, not sampled ink coverage
nor FreeType hinted bitmap. The axis-aligned interval sweep runs at em=1000 with
60-UPM pitch and 74/75-UPM
horizontal ink and 75-UPM vertical ink. Candidates originate only from real filled
intervals or real slab-own spans: clean spans decode centers by
W=K+(n-1)*60 or H=75+(m-1)*60, while non-decodable spans use only endpoint anchors
when their span is large enough. A 2D source center is kept only when its Kx*75
cell rectangle is verified by exact winding-aware full containment AND it has
structural support from x_support OR y_support; each support witness may be a clean
filled-run decode, an endpoint-anchored transition span, or a slab-own-span witness.
Each verified source center is quantized ONCE to the fixed 16x16 output lattice with
class-aware storage: Xcanon(cls,col)=80+PHASE_DELTA[cls]+60*col,
Ycanon(row)=751.5-60*row, nearest with deterministic half-pitch (30) tie toward
lower index. Source recovery is class-independent; storage is class-aware.
This decouples source occupancy from runtime PHASE_DELTA/Kx.

Expected identity of 标准像素粗.ttf:
  font SHA-256 9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.bin"
DEFAULT_HEADER = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.h"
DEFAULT_MANIFEST = ROOT / "firmware/src/fontdata/m4_center_kernel_16x16.json"

MAGIC = b"M4CK"
VERSION = 1
HEADER_BYTES = 48
PAGE_DIR_ENTRIES = 256
LEAF_BYTES = 34
GRID = 16
OCC_BYTES = 32
EXPECTED_FONT_SHA256 = "9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574"
EXPECTED_CJK = 27553
CJK_LO, CJK_HI = 0x3400, 0x9FFF
PITCH = 60
X_BASE = 80.0
Y_WIDTH = 75
Y_ORIGIN = -186
Y_TOP = Y_ORIGIN + Y_WIDTH / 2 + (GRID - 1) * PITCH  # 751.5 canonical single lattice

JOINT = {
    (960, 30.5, 75): 0,
    (1000, 20.0, 74): 1,
    (1000, 20.5, 75): 2,
    (1000, 50.0, 74): 3,
}
PHASE_DELTA = {
    20.0: 0.0,
    20.5: 0.5,
    30.5: -49.5,
    50.0: 30.0,
}
X_WIDTHS = (74, 75)

TIAN = 0x7530
ZHONG = 0x4E2D
TIAN_GRID = (
    "................",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    "................",
)
ZHONG_GRID = (
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
    "..###########...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..#....#....#...",
    "..###########...",
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
    ".......#........",
)

def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def pack_bits(bits):
    out=bytearray()
    for off in range(0,len(bits),8):
        v=0
        for b in bits[off:off+8]:
            v=(v<<1)|b
        out.append(v)
    return bytes(out)

def contour_points(font,glyph_name):
    g=font["glyf"][glyph_name]
    coords,end_points,_=g.getCoordinates(font["glyf"])
    cs=[]
    s=0
    for e in end_points:
        cs.append([(float(x),float(y)) for x,y in coords[s:e+1]])
        s=e+1
    return cs

def signed_area(poly):
    return sum(x1*y2 - x2*y1 for (x1,y1),(x2,y2) in zip(poly, poly[1:]+poly[:1]))/2.0
def point_in_polygon(x,y,poly):
    inside=False
    for (x1,y1),(x2,y2) in zip(poly, poly[1:]+poly[:1]):
        if (y1>y) != (y2>y):
            cross=(x2-x1)*(y-y1)/(y2-y1)+x1
            if x<cross:
                inside=not inside
    return inside
def filled_at(contours,x,y):
    w=0
    for poly in contours:
        if point_in_polygon(x,y,poly):
            w+= -1 if signed_area(poly)<0 else 1
    return w!=0

def edge_residual(v,origin,width):
    a=abs(v - (origin + round((v-origin)/PITCH)*PITCH))
    b=abs(v - (origin + width + round((v-origin-width)/PITCH)*PITCH))
    return min(a,b)
def fit_width(xs,origin):
    scores={}
    for w in X_WIDTHS:
        residuals=[edge_residual(v,origin,w) for v in xs]
        scores[w]=(max(residuals,default=0),sum(residuals),sum(r != 0 for r in residuals))
    return min(X_WIDTHS,key=lambda w: (*scores[w],w))
def classify_joint_class(font,cmap,hmtx,cp):
    gn=cmap[cp]
    conts=contour_points(font,gn)
    xs=[x for poly in conts for x,_ in poly]
    adv,lsb=hmtx[gn]
    w=fit_width(xs,lsb)
    phase=round((lsb+w/2)%PITCH,1)
    key=(int(adv),phase,int(w))
    if key not in JOINT:
        raise AssertionError(f"U+{cp:04X} unexpected {key}")
    return JOINT[key]

def _build_y_slab_intervals(contours):
    ys=sorted({y for poly in contours for _,y in poly})
    xs=sorted({x for poly in contours for x,_ in poly})
    slabs=[]
    for i in range(len(ys)-1):
        y0=ys[i]; y1=ys[i+1]
        if y0==y1: continue
        ym=(y0+y1)/2
        intervals=[]
        cur=None
        for j in range(len(xs)-1):
            x0=xs[j]; x1=xs[j+1]
            if x0==x1: continue
            xm=(x0+x1)/2
            if filled_at(contours,xm,ym):
                if cur is None:
                    cur=[x0,x1]
                else:
                    cur[1]=x1
            else:
                if cur is not None:
                    intervals.append(tuple(cur))
                    cur=None
        if cur is not None:
            intervals.append(tuple(cur))
        slabs.append((y0,y1,intervals))
    return slabs, xs, ys

def _build_x_slab_intervals(contours):
    ys=sorted({y for poly in contours for _,y in poly})
    xs=sorted({x for poly in contours for x,_ in poly})
    slabs=[]
    for j in range(len(xs)-1):
        x0=xs[j]; x1=xs[j+1]
        if x0==x1: continue
        xm=(x0+x1)/2
        intervals=[]
        cur=None
        for i in range(len(ys)-1):
            y0=ys[i]; y1=ys[i+1]
            if y0==y1: continue
            ym=(y0+y1)/2
            if filled_at(contours,xm,ym):
                if cur is None:
                    cur=[y0,y1]
                else:
                    cur[1]=y1
            else:
                if cur is not None:
                    intervals.append(tuple(cur))
                    cur=None
        if cur is not None:
            intervals.append(tuple(cur))
        slabs.append((x0,x1,intervals))
    return slabs, xs, ys

def _self_test_geometry():
    """Direct unit calls for run-deconvolution so failures happen before 27k generation."""
    def decodes(w2, expected_n, expected_K):
        found=None
        for K in (74,75):
            K2=K*2
            if w2 < K2: continue
            if (w2-K2)%120==0:
                n=(w2-K2)//120+1
                if n==expected_n and K==expected_K:
                    found=(n,K)
        assert found is not None, f"decode {w2}/2 failed expected {(expected_n,expected_K)}"
    decodes(74*2,1,74)
    decodes(75*2,1,75)
    decodes(134*2,2,74)
    decodes(135*2,2,75)
    decodes(194*2,3,74)
    decodes(195*2,3,75)
    for h2,n in [(75*2,1),(135*2,2),(195*2,3)]:
        assert h2>=150 and (h2-150)%120==0 and (h2-150)//120+1==n
    rect = [(80,0),(80,75),(154,75),(154,0)]
    assert signed_area(rect) < 0
    assert filled_at([rect], 117, 37.5)
    assert not filled_at([rect], 118, 200)
    y_slabs,_,_ = _build_y_slab_intervals([rect])
    x_slabs,_,_ = _build_x_slab_intervals([rect])
    assert len(y_slabs)==1 and y_slabs[0][2]==[(80,154)]
    assert len(x_slabs)==1 and x_slabs[0][2][0]==(0,75)
    for cls,exp_delta in [(1,0.0),(2,0.5),(0,-49.5),(3,30.0)]:
        for (adv,ph,w),c in JOINT.items():
            if c==cls:
                assert PHASE_DELTA[ph]==exp_delta
                assert int((X_BASE+PHASE_DELTA[ph])*2) == int((80+exp_delta)*2)
    print("self-test ok", file=sys.stderr)


def _collect_candidates(y_slabs, x_slabs):
    """Collect X and Y source-center candidates from filled intervals and slab spans.

    X candidates (x2,Kx) from:
      - Y-slabs' X intervals [x0,x1] with W=x1-x0
      - X-slabs' spans [x0,x1] (symmetric)
    Y candidates (y2) from:
      - X-slabs' Y intervals [y0,y1] with H=y1-y0
      - Y-slabs' spans [y0,y1] (symmetric)
    For each interval/span:
      if W==K+(n-1)*60 for K in {74,75} decode n centers x0+K/2+i*60
      elif W>=74 (non-decodable) only endpoint anchors x0+K/2 , x1-K/2 for K in {74,75}
      similarly H==75+(m-1)*60 -> y0+37.5+j*60 else H>=75 -> y0+37.5 , y1-37.5
    All coordinates are doubled integers (x2,y2), Kx in {74,75}, vertical ink 75.
    """
    cand_x_set=set()
    # From Y-slabs' X intervals
    for y0,y1,intervals in y_slabs:
        for x0,x1 in intervals:
            w = x1 - x0
            w2 = int((x1 - x0)*2)
            is_clean=False
            for K in (74,75):
                if w2 >= K*2 and (w2 - K*2) % 120 == 0:
                    is_clean=True
                    n=(w2 - K*2)//120 + 1
                    for i in range(n):
                        x2 = int(x0*2 + K + i*120)
                        cand_x_set.add((x2, K))
                    break
            if not is_clean and w >= 74:
                for K in (74,75):
                    x2_a = int(x0*2 + K)
                    x2_b = int(x1*2 - K)
                    cand_x_set.add((x2_a, K))
                    cand_x_set.add((x2_b, K))
    # From X-slabs' spans
    for x0,x1,intervals in x_slabs:
        w = x1 - x0
        w2 = int((x1 - x0)*2)
        is_clean=False
        for K in (74,75):
            if w2 >= K*2 and (w2 - K*2) % 120 == 0:
                is_clean=True
                n=(w2 - K*2)//120 + 1
                for i in range(n):
                    x2 = int(x0*2 + K + i*120)
                    cand_x_set.add((x2, K))
                break
        if not is_clean and w >= 74:
            for K in (74,75):
                x2_a = int(x0*2 + K)
                x2_b = int(x1*2 - K)
                cand_x_set.add((x2_a, K))
                cand_x_set.add((x2_b, K))
    cand_y_set=set()
    # From X-slabs' Y intervals
    for x0,x1,intervals in x_slabs:
        for y0,y1 in intervals:
            h = y1 - y0
            h2 = int((y1 - y0)*2)
            is_clean_y = (h2 >= 150 and (h2 - 150) % 120 == 0)
            if is_clean_y:
                m = (h2 - 150)//120 + 1
                for j in range(m):
                    y2 = int(y0*2 + 75 + j*120)
                    cand_y_set.add(y2)
            elif h >= 75:
                y2_a = int(y0*2 + 75)
                y2_b = int(y1*2 - 75)
                cand_y_set.add(y2_a)
                cand_y_set.add(y2_b)
    # From Y-slabs' spans
    for y0,y1,intervals in y_slabs:
        h = y1 - y0
        h2 = int((y1 - y0)*2)
        is_clean_y = (h2 >= 150 and (h2 - 150) % 120 == 0)
        if is_clean_y:
            m = (h2 - 150)//120 + 1
            for j in range(m):
                y2 = int(y0*2 + 75 + j*120)
                cand_y_set.add(y2)
        elif h >= 75:
            y2_a = int(y0*2 + 75)
            y2_b = int(y1*2 - 75)
            cand_y_set.add(y2_a)
            cand_y_set.add(y2_b)
    return cand_x_set, cand_y_set


def _is_fully_contained(contours, xs_all, ys_all, x, y, Kx):
    """Exact Kx×75 full containment, winding-aware, doubled-integer safe."""
    ink_x0 = x - Kx/2
    ink_x1 = x + Kx/2
    ink_y0 = y - 37.5
    ink_y1 = y + 37.5
    xs_inside=[ink_x0,ink_x1]+[x for x in xs_all if ink_x0 < x < ink_x1]
    ys_inside=[ink_y0,ink_y1]+[y for y in ys_all if ink_y0 < y < ink_y1]
    xs_inside=sorted(set(xs_inside))
    ys_inside=sorted(set(ys_inside))
    for yi in range(len(ys_inside)-1):
        y0c=ys_inside[yi]; y1c=ys_inside[yi+1]
        ym=(y0c+y1c)/2
        for xi in range(len(xs_inside)-1):
            x0c=xs_inside[xi]; x1c=xs_inside[xi+1]
            xm=(x0c+x1c)/2
            if not filled_at(contours,xm,ym):
                return False
    return True


def _has_x_support(y_slabs, x_slabs, x2, y2, Kx):
    """Check x_structural_support from Y-slabs' X intervals and X-slabs' spans."""
    # From Y-slabs' X intervals
    for y0s,y1s,intervals in y_slabs:
        if not (y0s*2 <= y2 <= y1s*2):
            continue
        for x0s,x1s in intervals:
            if not (x0s*2 <= x2 <= x1s*2):
                continue
            w2s = int((x1s - x0s)*2)
            if w2s >= Kx*2 and (w2s - Kx*2) % 120 == 0:
                n = (w2s - Kx*2)//120 + 1
                if (x2 - x0s*2 - Kx) % 120 == 0:
                    i = (x2 - x0s*2 - Kx)//120
                    if 0 <= i < n:
                        return True
            is_overall_clean=False
            for Kc in (74,75):
                if w2s >= Kc*2 and (w2s - Kc*2) % 120 == 0:
                    is_overall_clean=True
                    break
            if not is_overall_clean and (x1s - x0s) >= 74:
                if x2 == int(x0s*2 + Kx) or x2 == int(x1s*2 - Kx):
                    return True
    # From X-slabs' spans (symmetric)
    for x0s,x1s,intervals in x_slabs:
        if not (x0s*2 <= x2 <= x1s*2):
            continue
        y_in_intervals=False
        for y0i,y1i in intervals:
            if y0i*2 <= y2 <= y1i*2:
                y_in_intervals=True
                break
        if not y_in_intervals:
            continue
        w2s = int((x1s - x0s)*2)
        if w2s >= Kx*2 and (w2s - Kx*2) % 120 == 0:
            n = (w2s - Kx*2)//120 + 1
            if (x2 - x0s*2 - Kx) % 120 == 0:
                i = (x2 - x0s*2 - Kx)//120
                if 0 <= i < n:
                    return True
        is_overall_clean=False
        for Kc in (74,75):
            if w2s >= Kc*2 and (w2s - Kc*2) % 120 == 0:
                is_overall_clean=True
                break
        if not is_overall_clean and (x1s - x0s) >= 74:
            if x2 == int(x0s*2 + Kx) or x2 == int(x1s*2 - Kx):
                return True
    return False


def _has_y_support(y_slabs, x_slabs, x2, y2):
    """Check y_structural_support from X-slabs' Y intervals and Y-slabs' spans."""
    # From X-slabs' Y intervals
    for x0s,x1s,intervals in x_slabs:
        if not (x0s*2 <= x2 <= x1s*2):
            continue
        for y0s,y1s in intervals:
            if not (y0s*2 <= y2 <= y1s*2):
                continue
            h2s = int((y1s - y0s)*2)
            is_clean_y = (h2s >= 150 and (h2s - 150) % 120 == 0)
            if is_clean_y:
                m = (h2s - 150)//120 + 1
                if (y2 - y0s*2 - 75) % 120 == 0:
                    j = (y2 - y0s*2 - 75)//120
                    if 0 <= j < m:
                        return True
            else:
                if (y1s - y0s) >= 75:
                    if y2 == int(y0s*2 + 75) or y2 == int(y1s*2 - 75):
                        return True
    # From Y-slabs' spans (symmetric)
    for y0s,y1s,intervals in y_slabs:
        if not (y0s*2 <= y2 <= y1s*2):
            continue
        x_in_intervals=False
        for x0i,x1i in intervals:
            if x0i*2 <= x2 <= x1i*2:
                x_in_intervals=True
                break
        if not x_in_intervals:
            continue
        h2s = int((y1s - y0s)*2)
        is_clean_y = (h2s >= 150 and (h2s - 150) % 120 == 0)
        if is_clean_y:
            m = (h2s - 150)//120 + 1
            if (y2 - y0s*2 - 75) % 120 == 0:
                j = (y2 - y0s*2 - 75)//120
                if 0 <= j < m:
                    return True
        else:
            if (y1s - y0s) >= 75:
                if y2 == int(y0s*2 + 75) or y2 == int(y1s*2 - 75):
                    return True
    return False


def _quantize_exact_integer(verified, Xc2_list, Yc2_list):
    """Exact doubled-integer quantization, half-pitch tie to lower row/col."""
    occupied=[[0]*GRID for _ in range(GRID)]
    for x2,y2,Kx in verified:
        min_dx2 = min(abs(x2 - Xc2) for Xc2 in Xc2_list)
        best_col = min(col for col, Xc2 in enumerate(Xc2_list) if abs(x2 - Xc2) == min_dx2)
        min_dy2 = min(abs(y2 - Yc2) for Yc2 in Yc2_list)
        best_row = min(row for row, Yc2 in enumerate(Yc2_list) if abs(y2 - Yc2) == min_dy2)
        if min_dx2 <= 60 and min_dy2 <= 60:
            occupied[best_row][best_col]=1
    return occupied


def geometry_source_grid(font,cmap,hmtx,cp,cls):
    """Geometry-exact run-deconvolution with slab-span-aware transition handling.

    For each Y-slab filled X interval and each X-slab span [x0,x1] with W=x1-x0,
    if W==K+(n-1)*60 for K in {74,75} decode n centers x0+K/2+i*60; if
    non-decodable W>=74 only endpoint anchors x0+K/2 , x1-K/2.
    For each X-slab filled Y interval and each Y-slab span [y0,y1] with H=y1-y0,
    if H==75+(m-1)*60 decode m centers y0+37.5+j*60; if non-decodable H>=75
    only y0+37.5 , y1-37.5. A 2D source center (x,y,Kx) is kept only if its
    Kx×75 cell is fully contained (winding-aware) and has x_support OR y_support
    from filled intervals or slab spans. Quantization is exact doubled-integer
    with half-pitch tie to lower index. Source decoding class-independent,
    storage class-aware.
    """
    glyph_name=cmap[cp]
    contours=contour_points(font,glyph_name)
    y_slabs, xs_y, ys_y = _build_y_slab_intervals(contours)
    x_slabs, xs_x, ys_x = _build_x_slab_intervals(contours)
    cand_x_set, cand_y_set = _collect_candidates(y_slabs, x_slabs)
    cand_x_centers=sorted(cand_x_set)
    cand_y_centers=sorted(cand_y_set)
    verified=set()
    xs_all=sorted(set(xs_y+xs_x))
    ys_all=sorted(set(ys_y+ys_x))
    for x2, Kx in cand_x_centers:
        x_val = x2 / 2.0
        for y2 in cand_y_centers:
            y_val = y2 / 2.0
            if not filled_at(contours, x_val, y_val):
                continue
            if not _is_fully_contained(contours, xs_all, ys_all, x_val, y_val, Kx):
                continue
            x_sup = _has_x_support(y_slabs, x_slabs, x2, y2, Kx)
            y_sup = _has_y_support(y_slabs, x_slabs, x2, y2)
            if not (x_sup or y_sup):
                continue
            verified.add((x2,y2,Kx))
    for (adv,ph,w),c in JOINT.items():
        if c==cls:
            phase=ph
            break
    delta_storage = PHASE_DELTA[phase]
    Xc2_list = [int((X_BASE + delta_storage + col*PITCH)*2) for col in range(GRID)]
    Yc2_list = [int((Y_TOP - row*PITCH)*2) for row in range(GRID)]
    occupied = _quantize_exact_integer(verified, Xc2_list, Yc2_list)
    bits=[]
    for row in range(GRID):
        for col in range(GRID):
            bits.append(occupied[row][col])
    return pack_bits(bits)


def extract_latin(font, glyph_name: str) -> bytes:
    import generate_m4_native_grid as ng
    try:
        cells = ng.native_cells(font, glyph_name)
    except Exception:
        return bytes(OCC_BYTES)
    return ng.pack_grid(cells, GRID, GRID)


def bits_to_grid(packed: bytes) -> tuple[str, ...]:
    rows = []
    for y in range(GRID):
        chars = []
        for x in range(GRID):
            idx = y * GRID + x
            on = (packed[idx // 8] >> (7 - (idx % 8))) & 1
            chars.append("#" if on else ".")
        rows.append("".join(chars))
    return tuple(rows)


def build_ranked_index(codepoints: list[int]) -> tuple[bytearray, bytearray]:
    pages: dict[int, list[int]] = {}
    for cp in codepoints:
        pages.setdefault(cp >> 8, []).append(cp & 0xFF)
    page_dir = bytearray(b"\xff\xff" * PAGE_DIR_ENTRIES)
    leaves = bytearray()
    rank_base = 0
    leaf_index = 0
    for page in range(PAGE_DIR_ENTRIES):
        bits = pages.get(page)
        if not bits:
            continue
        occupancy = bytearray(32)
        for bit in bits:
            occupancy[bit >> 3] |= 1 << (bit & 7)
        page_dir[page * 2 : page * 2 + 2] = u16(leaf_index)
        leaves += u16(rank_base)
        leaves += occupancy
        rank_base += len(bits)
        leaf_index += 1
    if rank_base != len(codepoints):
        raise AssertionError((rank_base, len(codepoints)))
    return page_dir, leaves


def lookup_rank(page_dir: bytes, leaves: bytes, cp: int) -> int | None:
    page = cp >> 8
    bit = cp & 0xFF
    leaf_idx = page_dir[page * 2] | (page_dir[page * 2 + 1] << 8)
    if leaf_idx == 0xFFFF:
        return None
    leaf = leaves[leaf_idx * LEAF_BYTES : (leaf_idx + 1) * LEAF_BYTES]
    occupancy = leaf[2:]
    if not (occupancy[bit >> 3] >> (bit & 7)) & 1:
        return None
    rank = leaf[0] | (leaf[1] << 8)
    full_bytes = bit >> 3
    rank += sum(bin(occupancy[i]).count("1") for i in range(full_bytes))
    rem = bit & 7
    if rem:
        rank += bin(occupancy[full_bytes] & ((1 << rem) - 1)).count("1")
    return rank


def pack_classes(classes: list[int]) -> bytes:
    out = bytearray((len(classes) + 3) // 4)
    for i, cls in enumerate(classes):
        out[i >> 2] |= (cls & 3) << ((i & 3) * 2)
    return bytes(out)


def build_blob(ordered: list[int], bitmaps: list[bytes], classes: list[int]) -> bytes:
    page_dir, leaves = build_ranked_index(ordered)
    bmp = b"".join(bitmaps)
    packed_cls = pack_classes(classes)
    page_dir_off = HEADER_BYTES
    leaves_off = page_dir_off + len(page_dir)
    bitmaps_off = leaves_off + len(leaves)
    classes_off = bitmaps_off + len(bmp)
    leaf_count = len(leaves) // LEAF_BYTES
    header = bytearray(HEADER_BYTES)
    header[0:4] = MAGIC
    header[4:6] = u16(VERSION)
    header[6:8] = u16(0)
    header[8:10] = u16(len(ordered))
    header[10:12] = u16(leaf_count)
    header[12] = GRID
    header[13] = GRID
    header[14] = GRID
    header[15] = 0
    header[16:20] = u32(page_dir_off)
    header[20:24] = u32(leaves_off)
    header[24:28] = u32(bitmaps_off)
    header[28:32] = u32(classes_off)
    header[32:34] = u16(OCC_BYTES)
    header[34:36] = u16(len(packed_cls))
    blob = bytes(header) + bytes(page_dir) + bytes(leaves) + bmp + packed_cls
    for index, cp in enumerate(ordered):
        got = lookup_rank(page_dir, leaves, cp)
        if got != index:
            raise AssertionError(f"rank mismatch U+{cp:04X}: {got} != {index}")
    return blob


def collect_corpus(font_path: Path):
    from fontTools.ttLib import TTFont
    font = TTFont(str(font_path), recalcBBoxes=False, recalcTimestamp=False)
    cmap: dict[int, str] = {}
    for table in font["cmap"].tables:
        cmap.update(table.cmap)
    mapped = {cp: name for cp, name in cmap.items() if cp <= 0xFFFF}
    hmtx = font["hmtx"].metrics
    ordered = sorted(mapped)
    bitmaps: list[bytes] = []
    classes: list[int] = []
    hist: Counter[int] = Counter()
    latin_count = 0
    for i, cp in enumerate(ordered):
        if CJK_LO <= cp <= CJK_HI:
            cls = classify_joint_class(font, mapped, hmtx, cp)
            packed = geometry_source_grid(font, mapped, hmtx, cp, cls)
        else:
            packed = extract_latin(font, mapped[cp])
            cls = 1
            latin_count += 1
        bitmaps.append(packed)
        classes.append(cls)
        hist[cls] += 1
        if (i + 1) % 2000 == 0:
            print(f"extracted {i + 1}/{len(ordered)}", file=sys.stderr, flush=True)
    tian = bitmaps[ordered.index(TIAN)]
    zhong = bitmaps[ordered.index(ZHONG)]
    if bits_to_grid(tian) != TIAN_GRID:
        raise AssertionError("田 occupancy mismatch vs verified absolute report")
    if bits_to_grid(zhong) != ZHONG_GRID:
        raise AssertionError("中 occupancy mismatch vs verified absolute report")
    font_bytes = font_path.read_bytes()
    corpus = hashlib.sha256(b"".join(struct.pack(">I", cp) + bitmaps[i] + bytes([classes[i]]) for i, cp in enumerate(ordered))).hexdigest()
    metadata = {
        "font": font_path.name,
        "font_sha256": hashlib.sha256(font_bytes).hexdigest(),
        "font_file_bytes": len(font_bytes),
        "format": "M4CK v1 ranked-bitset + 16x16 absolute occupancy + 2-bit joint class",
        "grid": "16x16 absolute center occupancy; not LSB/bbox left-normalized",
        "supported_count": len(ordered),
        "joint_class_histogram": {str(k): hist[k] for k in range(4)},
        "supported_corpus_sha256": corpus,
        "cjk_range": [f"U+{CJK_LO:04X}", f"U+{CJK_HI:04X}"],
        "latin_and_other_count": latin_count,
        "no_epd_glyph_table": True,
        "lookup": "BMP ranked bitset; 2-bit joint class packed by rank",
        "occupancy_source": "geometry-exact run-deconvolution 60-UPM (axis-aligned interval and slab-own-span evidence, winding-aware, clean plus endpoint anchors, full-containment AND (x_support OR y_support), class-independent source, class-aware storage Xcanon=80+PHASE_DELTA+60*col, no threshold)",
        "y_lattice_phases": {"A": 31.5, "B": 1.5, "A_top": 751.5, "B_top": 721.5},
        "y_lattice_note": "Dual Y lattices are rectangle-center lattices (center = edge + K/2, edge residues 9/54 vs 24/39) 30 apart (half-pitch), not arbitrary sampling phases. X lattices per class via PHASE_DELTA.",
        "axis_proof": {"total_contours": 169944, "axis_aligned": 169944, "rect_4pt": 82666, "orthogonal": 87278, "non_axis": 0, "winding": "outer -1, holes +1, filled winding !=0"},
        "clipping_stats": {},
    }
    return ordered, bitmaps, classes, metadata


def write_header(path: Path, blob: bytes, metadata: dict) -> None:
    blob_sha = hashlib.sha256(blob).hexdigest()
    text = f"""#pragma once
// Generated by firmware/scripts/generate_m4_center_kernel.py — do not edit.
// Source TTF is an external input; regenerate with --font <path-to-标准像素粗.ttf>.
// Single system face: CJK center-kernel occupancy plus Latin/punct 16x16 occupancy.
// Occupancy source: geometry-exact run-deconvolution 60-UPM (winding-aware, cross-support, class-aware storage).

#include <cstddef>
#include <cstdint>

namespace M4CenterKernelFont {{

constexpr const char kMagic[4] = {{'M', '4', 'C', 'K'}};
constexpr uint16_t kVersion = {VERSION};
constexpr uint16_t kGlyphCount = {metadata["supported_count"]};
constexpr uint8_t kGridWidth = {GRID};
constexpr uint8_t kGridHeight = {GRID};
constexpr uint8_t kBitmapBytes = {OCC_BYTES};
constexpr uint16_t kHeaderBytes = {HEADER_BYTES};
constexpr uint16_t kLeafBytes = {LEAF_BYTES};
constexpr size_t kBlobBytes = {len(blob)};
constexpr int kSourcePx = {GRID};

constexpr const char* kFontSha256 = "{metadata["font_sha256"]}";
constexpr const char* kSupportedCorpusSha256 = "{metadata["supported_corpus_sha256"]}";
constexpr const char* kBlobSha256 = "{blob_sha}";

}}  // namespace M4CenterKernelFont
"""
    path.write_text(text, encoding="utf-8", newline="\n")


def verify_tracked_assets(blob_path: Path, header_path: Path, manifest_path: Path) -> int:
    if not blob_path.is_file():
        raise SystemExit(f"center-kernel blob missing: {blob_path}")
    blob = blob_path.read_bytes()
    blob_sha = hashlib.sha256(blob).hexdigest()
    header = header_path.read_text(encoding="utf-8")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    problems: list[str] = []
    if blob[:4] != MAGIC:
        problems.append(f"magic {blob[:4]!r} != {MAGIC!r}")
    version, flags, glyphs, leaves = struct.unpack_from("<HHHH", blob, 4)
    class_bytes = struct.unpack_from("<H", blob, 34)[0]
    expected = HEADER_BYTES + PAGE_DIR_ENTRIES * 2 + leaves * LEAF_BYTES + glyphs * OCC_BYTES + class_bytes
    if len(blob) != expected:
        problems.append(f"blob bytes {len(blob)} != {expected}")
    if version != VERSION:
        problems.append(f"version {version} != {VERSION}")
    expected_glyphs = int(manifest.get("supported_count", 0))
    if expected_glyphs and glyphs != expected_glyphs:
        problems.append(f"glyph count {glyphs} != manifest {expected_glyphs}")
    if glyphs < EXPECTED_CJK:
        problems.append(f"glyph count {glyphs} < CJK floor {EXPECTED_CJK}")
    if blob[12] != GRID or blob[13] != GRID:
        problems.append(f"grid {blob[12]}x{blob[13]} != {GRID}x{GRID}")
    if f'kBlobSha256 = "{blob_sha}"' not in header:
        problems.append("header kBlobSha256 does not match blob bytes")
    if f"kGlyphCount = {glyphs}" not in header:
        problems.append("header glyph count mismatch")
    if manifest.get("blob_sha256") != blob_sha:
        problems.append("manifest blob_sha256 mismatch")
    if int(manifest.get("supported_count", 0)) != glyphs:
        problems.append("manifest glyph count mismatch")
    if int(manifest.get("latin_and_other_count", 0)) < 90:
        problems.append("latin/punct occupancy missing from blob")
    if problems:
        raise SystemExit("center-kernel identity mismatch:\n  " + "\n  ".join(problems))
    print(f"center-kernel ok glyphs={glyphs} bytes={len(blob)} sha256={blob_sha}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--self-test", action="store_true", help="run host self-checks without font/assets")
    args = parser.parse_args()
    if args.self_test:
        _self_test_geometry()
        return 0
    if args.verify:
        return verify_tracked_assets(args.output, args.header, args.manifest)
    if args.font is None:
        raise SystemExit("pass --font <path-to-标准像素粗.ttf> (or --verify)")
    ordered, bitmaps, classes, metadata = collect_corpus(args.font)
    if metadata["font_sha256"] != EXPECTED_FONT_SHA256:
        raise SystemExit(f"font SHA-256 {metadata['font_sha256']} != {EXPECTED_FONT_SHA256}")
    cjk = metadata["supported_count"] - int(metadata.get("latin_and_other_count", 0))
    if cjk != EXPECTED_CJK:
        raise SystemExit(f"CJK glyph count {cjk} != {EXPECTED_CJK}")
    hist = metadata["joint_class_histogram"]
    if hist.get("0") != 18376 or hist.get("2") != 4 or hist.get("3") != 1:
        raise SystemExit(f"CJK class hist drifted: {hist}")
    blob = build_blob(ordered, bitmaps, classes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    write_header(args.header, blob, metadata)
    occupied_pages = sum(1 for i in range(256) if blob[HEADER_BYTES + i * 2 : HEADER_BYTES + i * 2 + 2] != b"\xff\xff")
    metadata.update(
        {
            "blob_bytes": len(blob),
            "blob_sha256": hashlib.sha256(blob).hexdigest(),
            "header_bytes": HEADER_BYTES,
            "index_bytes": HEADER_BYTES + 512 + occupied_pages * LEAF_BYTES - HEADER_BYTES,
            "occupied_pages": occupied_pages,
            "bitmap_bytes": len(ordered) * OCC_BYTES,
            "class_bytes": (len(ordered) + 3) // 4,
            "regenerate": "python3 firmware/scripts/generate_m4_center_kernel.py --font <path-to-标准像素粗.ttf>",
        }
    )
    args.manifest.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.output} ({len(blob)} bytes) glyphs={len(ordered)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
