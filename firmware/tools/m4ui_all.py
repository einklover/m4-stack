#!/usr/bin/env python3
"""Murphy M4 全 UI 预览台 — 覆盖所有 50+ 活动页面。

从 firmware/src/activities/ 中提取每一页的 UI 布局，渲染为 480×800 PNG。
复用 m4ui_preview.py 的字体/尺寸/辅助函数。

用法:
  python tools/m4ui_all.py                          # 所有页面
  python tools/m4ui_all.py --screen home             # 单页
  python tools/m4ui_all.py --screen all --out build/m4ui-preview
  python tools/m4ui_all.py --watch                  # 持续重建
  python tools/m4ui_all.py --font C:/Windows/Fonts/simsun.ttc
"""
from __future__ import annotations

import argparse
import shutil
import time
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from sui_icon_cache import ICONS as SUI_ICONS, blit as draw_sui_icon  # ported SimpleUI icons

# ── 复用 m4ui_preview.py 的尺寸/字体常量 ──────────────────────
W, H = 480, 800
PAD = 30
HEADER_H = 50
FOOTER_H = 46

# Theme color roles
CLR_BG=255
CLR_FG=0
CLR_CARD=242
CLR_SELECTED=237
CLR_SUBTITLE=128
CLR_DIVIDER=215
CLR_FOOTER_BG=243
CLR_GRID=244
CLR_BAR_BG=220
CLR_BAR_FG=0
GAP = 6
SMALL_PX = 18
UI_10_PX = 22
UI_12_PX = 26
READER_BODY_PX = 28

FONT_CANDIDATES = [
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/STZHONGS.TTF",
]
BOLD_CANDIDATES = [
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/Dengb.ttf",
    "C:/Windows/Fonts/simsun.ttc",
]

def first_existing(paths):
    for p in paths:
        if Path(p).exists():
            return p
    raise SystemExit("No CJK font found.")

REGULAR = first_existing(FONT_CANDIDATES)
BOLD = first_existing(BOLD_CANDIDATES)

def face(px, bold=False):
    return ImageFont.truetype(BOLD if bold else REGULAR, px)

def ui10(bold=False): return face(UI_10_PX, bold)
def ui12(bold=False): return face(UI_12_PX, bold)
def small(bold=False): return face(SMALL_PX, bold)

def tw(draw, text, f):
    return int(draw.textlength(text, font=f))

def fit(draw, text, f, width):
    if tw(draw, text, f) <= width:
        return text
    while text and tw(draw, text + "...", f) > width:
        text = text[:-1]
    return text + "..."

def new_canvas():
    img = Image.new("L", (W, H), 255)
    return img, ImageDraw.Draw(img)

def draw_header(draw, title):
    f = ui12(True)
    draw.text((PAD, 8), fit(draw, title, f, W - 2*PAD - 55), font=f, fill=0)
    draw.line((0, HEADER_H - 2, W, HEADER_H - 2), fill=CLR_DIVIDER, width=1)
    draw.text((W - 48, 13), "87%", font=small(), fill=0)

def draw_footer(draw, labels):
    y = H - FOOTER_H
    draw.rectangle((0, y, W, H), fill=CLR_FOOTER_BG)
    draw.line((0, y, W, y), fill=CLR_DIVIDER)
    n = len(labels)
    if n == 0: return
    cw = W // n
    for i, lbl in enumerate(labels):
        x = cw * i + (cw - tw(draw, lbl, small())) // 2
        draw.text((x, y + 12), lbl, font=small(), fill=CLR_FG)

def draw_list(draw, items, y_start, row_h=56, selected=0, sub=None):
    """items: list of str; sub: optional list of str for subtitle."""
    y = y_start
    for i, item in enumerate(items):
        if y + row_h > H - FOOTER_H:
            break
        if i == selected:
            draw.rounded_rectangle((PAD - 5, y - 2, W - PAD + 5, y + row_h - 4), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y + 8), item, font=ui10(i == selected), fill=0)
        if sub and i < len(sub):
            draw.text((PAD, y + 32), sub[i], font=small(), fill=90)
        y += row_h

def draw_tab_bar(draw, tabs, y):
    """tabs: list of (label, selected)."""
    x = PAD
    for label, sel in tabs:
        tw_ = tw(draw, label, ui10(True))
        bw = tw_ + 24
        if sel:
            draw.rounded_rectangle((x - 2, y - 2, x + bw, y + 32), radius=4, fill=0)
            draw.text((x + 12, y + 4), label, font=ui10(True), fill=255)
        else:
            draw.rounded_rectangle((x - 2, y - 2, x + bw, y + 32), radius=4, fill=230)
            draw.text((x + 12, y + 4), label, font=ui10(True), fill=0)
        x += bw + 8

def draw_progress_bar(draw, pct, x, y, w, h=6):
    """pct: 0..100"""
    draw.rounded_rectangle((x, y, x + w, y + h), radius=3, fill=220)
    fw = int(w * pct / 100)
    if fw > 0:
        draw.rounded_rectangle((x, y, x + fw, y + h), radius=3, fill=0)


# ── 屏幕渲染器 ──────────────────────────────────────────────────

def draw_progress_ring(draw,cx,cy,r,pct,th=8):
 draw.ellipse([cx-r,cy-r,cx+r,cy+r],outline=CLR_BAR_BG,width=th)
 if pct>0:
  start=-90;end=-90+3.6*pct
  draw.arc([cx-r,cy-r,cx+r,cy+r],start,end,fill=CLR_BAR_FG,width=th)
 l=str(int(pct))+chr(37)
 tw=int(draw.textlength(l,font=ui12(True)))
 draw.text((cx-tw//2,cy-10),l,font=ui12(True),fill=CLR_FG)

def render_home():
    """Home -- reading card + goal ring + menu grid"""
    img, draw = new_canvas()
    draw_header(draw, "")
    y = HEADER_H + 10
    # -- Currently Reading card --
    card_h = 110
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+card_h), radius=8, fill=CLR_CARD)
    # cover placeholder
    cx, cy = PAD + 5, y + 14
    draw.rounded_rectangle((cx, cy, cx+64, cy+80), radius=4, fill=CLR_BG)
    draw_sui_icon(img, "library", cx + 16, cy + 24, 32)
    # text column
    tx = cx + 80
    draw.text((tx, y + 16), "\u4e09\u4f53", font=ui12(True), fill=CLR_FG)
    draw.text((tx, y + 44), "\u5218\u6148\u6b23", font=small(), fill=CLR_SUBTITLE)
    # progress bar
    bar_y = y + 70; bar_w = W - tx - PAD - 5
    draw.rounded_rectangle((tx, bar_y, tx+bar_w, bar_y+8), radius=4, fill=CLR_BAR_BG)
    fw = int(bar_w * 0.75)
    draw.rounded_rectangle((tx, bar_y, tx+fw, bar_y+8), radius=4, fill=CLR_BAR_FG)
    draw.text((tx, bar_y + 14), "75% \u00b7 \u5df2\u8bfb 245 \u9875", font=small(), fill=CLR_SUBTITLE)
    # -- Reading Goal ring --
    y += card_h + 16
    rr = 40
    draw_progress_ring(draw, PAD + rr + 10, y + rr + 5, rr, 60, th=7)
    draw.text((PAD + 2*rr + 20, y + 8), "\u5e74\u5ea6\u9605\u8bfb\u76ee\u6807", font=ui10(True), fill=CLR_FG)
    draw.text((PAD + 2*rr + 20, y + 32), "12 / 20 \u672c", font=small(), fill=CLR_SUBTITLE)
    draw.text((PAD + 2*rr + 20, y + 52), "\u5df2\u8bfb 3647 \u9875", font=small(), fill=CLR_SUBTITLE)
    y += 2*rr + 10
    # -- Divider --
    draw.line((PAD, y, W-PAD, y), fill=CLR_DIVIDER)
    y += 10
    # -- 2x3 menu grid --
    items = ["\u6211\u7684\u4e66\u67b6", "\u6700\u8fd1\u9605\u8bfb", "\u6587\u4ef6\u4f20\u8f93", "\u5e94\u7528", "\u7f51\u7edc\u7ba1\u7406", "\u7cfb\u7edf\u8bbe\u7f6e"]
    icons = ["library", "history", "random", "plugin", "default", "settings"]
    cols, rows = 3, 2
    cw = (W - 2*PAD) // cols
    for i, (item, icon) in enumerate(zip(items, icons)):
        gx = PAD + (i % cols) * cw
        gy = y + (i // cols) * 70
        draw.rounded_rectangle((gx, gy, gx + cw - 8, gy + 58), radius=6, fill=CLR_GRID)
        draw_sui_icon(img, icon, gx + 10, gy + 12, 34)
        draw.text((gx + 52, gy + 18), item, font=ui10(), fill=CLR_FG)
    draw_footer(draw, ["", "\u9009\u62e9", "\u4e0a", "\u4e0b"])
    return img

def render_my_library():
    """MyLibrary -- 文件浏览器"""
    img, draw = new_canvas()
    draw_header(draw, "我的书架")
    y = HEADER_H + 14
    files = [
        ("小说/", "12 个文件", "collections"),
        ("历史/", "8 个文件", "collections"),
        ("科技/", "15 个文件", "collections"),
        ("哲学/", "5 个文件", "collections"),
        ("三体：黑暗森林.epub", "28.5MB · 刘慈欣", "library"),
        ("活着.txt", "512KB · 余华", "library"),
        ("百年孤独.epub", "18.2MB · 马尔克斯", "library"),
        ("人类简史.xtc", "24.1MB · 赫拉利", "library"),
        ("道德经.txt", "128KB · 老子", "library")
    ]
    row_h = 54
    for i, (name, sub, ic) in enumerate(files):
        if y + row_h > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+row_h-4), radius=6, fill=CLR_SELECTED)
        draw_sui_icon(img, ic, PAD + 2, y + 9, 30)
        draw.text((PAD + 40, y + 5), name, font=ui10(i==0), fill=0)
        draw.text((PAD + 40, y + 30), sub, font=small(), fill=90)
        y += row_h

    return img
def render_recent_books():
    """RecentBooks — 最近阅读"""
    img, draw = new_canvas()
    draw_header(draw, "最近阅读")
    y = HEADER_H + 14
    books = [
        ("三体", "75% · 刘慈欣"),
        ("活着", "32% · 余华"),
        ("百年孤独", "88% · 马尔克斯"),
        ("人类简史", "45% · 赫拉利"),
        ("道德经", "12% · 老子"),
        ("时间简史", "60% · 霍金"),
        ("围城", "3% · 钱钟书"),
        ("红楼梦", "28% · 曹雪芹"),
    ]
    row_h = 56
    for i, (title, info) in enumerate(books):
        if y + row_h > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+row_h-4), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), title, font=ui10(i==0), fill=0)
        draw.text((PAD, y+30), info, font=small(), fill=90)
        draw_progress_bar(draw, int(books[i][1].split("%")[0]), W-PAD-100, y+12, 90, 4)
        y += row_h
    draw_footer(draw, ["返回", "打开", "上", "下"])
    return img


# ── Reader Screens ──────────────────────────────────────────────

def render_reader_epub():
    """EPUB Reader — 正文阅读页"""
    img, draw = new_canvas()
    y = HEADER_H + 8
    # Currently Reading card
    card_h = 75
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+card_h), radius=8, fill=CLR_CARD)
    draw_sui_icon(img, "library", PAD+8, y+20, 34)
    draw.text((PAD+50, y+10), "三体", font=ui12(True), fill=CLR_FG)
    draw.text((PAD+50, y+34), "刘慈欣 · 75%", font=small(), fill=CLR_SUBTITLE)
    bar_w = W - PAD - 50 - PAD - 5
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+bar_w, y+58), radius=4, fill=CLR_BAR_BG)
    fw = int(bar_w * 0.75)
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+fw, y+58), radius=4, fill=CLR_BAR_FG)
    text = (
        "第一章\n\n"
        "　　宇宙就是一座黑暗森林，每个文明都是带枪的猎人，像幽灵般潜行于林间，"
        "轻轻拨开挡路的树枝，竭力不让脚步发出一点儿声音，连呼吸都必须小心翼翼。\n\n"
        "　　他必须小心，因为林中到处都有与他一样潜行的猎人。如果他发现了别的生命，"
        "能做的只有一件事：开枪消灭之。在这片森林中，他人就是地狱，就是永恒的威胁。"
    )
    f = face(READER_BODY_PX)
    draw.text((PAD, y + card_h + 10), text, font=f, fill=0, spacing=8)
    # 阅读进度条
    draw_progress_bar(draw, 75, 0, H-3, W, 3)
    draw_progress_ring(draw, W-50, H-35, 20, 75, th=4)
    draw_footer(draw, ["", "", "←", "→"])
    return img

def render_reader_xtc():
    """XTC Reader — 正文阅读页"""
    img, draw = new_canvas()
    y = HEADER_H + 8
    # Currently Reading card
    card_h = 75
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+card_h), radius=8, fill=CLR_CARD)
    draw_sui_icon(img, "library", PAD+8, y+20, 34)
    draw.text((PAD+50, y+10), "活着", font=ui12(True), fill=CLR_FG)
    draw.text((PAD+50, y+34), "余华 · 32%", font=small(), fill=CLR_SUBTITLE)
    bar_w = W - PAD - 50 - PAD - 5
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+bar_w, y+58), radius=4, fill=CLR_BAR_BG)
    fw = int(bar_w * 0.32)
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+fw, y+58), radius=4, fill=CLR_BAR_FG)
    text = (
        "活着\n\n"
        "　　我比现在年轻十岁的时候，觉得活着就是活着，"
        "没有什么特别的意义。后来我才明白，活着本身就是最大的意义。\n\n"
        "　　人是为了活着本身而活着的，而不是为了活着之外的任何事物而活着。"
        "我认识一个老人，他叫福贵，他的一生，就是一本书。"
    )
    f = face(READER_BODY_PX)
    draw.text((PAD, y + card_h + 10), text, font=f, fill=0, spacing=8)
    draw_progress_bar(draw, 32, 0, H-3, W, 3)
    draw_progress_ring(draw, W-50, H-35, 20, 32, th=4)
    draw_footer(draw, ["", "", "←", "→"])
    return img

def render_reader_txt():
    """TXT Reader — 正文阅读页"""
    img, draw = new_canvas()
    y = HEADER_H + 8
    # Currently Reading card
    card_h = 75
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+card_h), radius=8, fill=CLR_CARD)
    draw_sui_icon(img, "library", PAD+8, y+20, 34)
    draw.text((PAD+50, y+10), "道德经", font=ui12(True), fill=CLR_FG)
    draw.text((PAD+50, y+34), "老子 · 12%", font=small(), fill=CLR_SUBTITLE)
    bar_w = W - PAD - 50 - PAD - 5
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+bar_w, y+58), radius=4, fill=CLR_BAR_BG)
    fw = int(bar_w * 0.12)
    draw.rounded_rectangle((PAD+50, y+50, PAD+50+fw, y+58), radius=4, fill=CLR_BAR_FG)
    text = (
        "道德经 · 第一章\n\n"
        "　　道可道，非常道；名可名，非常名。\n"
        "　　无名天地之始，有名万物之母。\n"
        "　　故常无欲，以观其妙；常有欲，以观其徼。\n"
        "　　此两者同出而异名，同谓之玄。玄之又玄，众妙之门。\n\n"
        "　　第二章\n\n"
        "　　天下皆知美之为美，斯恶已；皆知善之为善，斯不善已。"
    )
    f = face(READER_BODY_PX)
    draw.text((PAD, y + card_h + 10), text, font=f, fill=0, spacing=8)
    draw_progress_bar(draw, 12, 0, H-3, W, 3)
    draw_progress_ring(draw, W-50, H-35, 20, 12, th=4)
    draw_footer(draw, ["", "", "←", "→"])
    return img

# ── Reader Menus ────────────────────────────────────────────────

def render_reader_menu_epub():
    """EPUB Reader Menu — 阅读菜单"""
    img, draw = new_canvas()
    draw_header(draw, "三体")
    y = HEADER_H + 14
    items = [
        "目录",
        "书签",
        "阅读设置",
        "字体设置",
        "搜索",
        "跳转到...",
        "分享",
    ]
    for i, item in enumerate(items):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+8), item, font=ui10(i==0), fill=0)
        y += 48
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img

def render_reader_menu_xtc():
    """XTC Reader Menu — XTC阅读菜单"""
    img, draw = new_canvas()
    draw_header(draw, "活着")
    y = HEADER_H + 14
    items = [
        "目录",
        "书签管理",
        "阅读设置",
        "翻页设置",
        "进度跳转",
    ]
    for i, item in enumerate(items):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+8), item, font=ui10(i==0), fill=0)
        y += 48
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img

def render_chapter_epub():
    """EPUB Chapter Selection — 章节选择"""
    img, draw = new_canvas()
    draw_header(draw, "目录 · 三体")
    y = HEADER_H + 14
    chapters = [
        ("第一章 科学边界", "第1页"),
        ("第二章 台球", "第15页"),
        ("第三章 射手和农场主", "第28页"),
        ("第四章 三体问题", "第42页"),
        ("第五章 红岸基地", "第58页"),
        ("第六章 叶文洁", "第76页"),
        ("第七章 三体游戏", "第95页"),
        ("第八章 面壁者", "第120页"),
    ]
    for i, (ch, pg) in enumerate(chapters):
        if i >= 8: break
        yp = y + i * 48
        if yp + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, yp-2, W-PAD+5, yp+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, yp+5), ch, font=ui10(i==0), fill=0)
        draw.text((W-PAD-60, yp+5), pg, font=small(), fill=130)
        if i < len(chapters) - 1:
            draw.line((PAD, yp+44, W-PAD, yp+44), fill=235)
    draw_footer(draw, ["返回", "跳转", "上", "下"])
    return img

def render_chapter_xtc():
    """XTC Chapter Selection"""
    img, draw = new_canvas()
    draw_header(draw, "目录 · 活着")
    y = HEADER_H + 14
    chapters = [
        ("第一章 少年", "第1页"),
        ("第二章 败家", "第23页"),
        ("第三章 从军", "第45页"),
        ("第四章 归来", "第67页"),
        ("第五章 改革", "第89页"),
        ("第六章 暮年", "第110页"),
    ]
    for i, (ch, pg) in enumerate(chapters):
        yp = y + i * 48
        if yp + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, yp-2, W-PAD+5, yp+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, yp+5), ch, font=ui10(i==0), fill=0)
        draw.text((W-PAD-60, yp+5), pg, font=small(), fill=130)
        draw.line((PAD, yp+44, W-PAD, yp+44), fill=235)
    draw_footer(draw, ["返回", "跳转", "上", "下"])
    return img

def render_chapter_txt():
    """TXT Chapter Selection"""
    img, draw = new_canvas()
    draw_header(draw, "目录 · 道德经")
    y = HEADER_H + 14
    chapters = [
        ("第一章 道可道", "第1页"),
        ("第二章 天下皆知", "第3页"),
        ("第三章 不尚贤", "第5页"),
        ("第四章 道冲而用之", "第7页"),
        ("第五章 天地不仁", "第9页"),
        ("第六章 谷神不死", "第11页"),
        ("第七章 天长地久", "第13页"),
        ("第八章 上善若水", "第15页"),
    ]
    for i, (ch, pg) in enumerate(chapters):
        yp = y + i * 48
        if yp + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, yp-2, W-PAD+5, yp+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, yp+5), ch, font=ui10(i==0), fill=0)
        draw.text((W-PAD-60, yp+5), pg, font=small(), fill=130)
        draw.line((PAD, yp+44, W-PAD, yp+44), fill=235)
    draw_footer(draw, ["返回", "跳转", "上", "下"])
    return img


# ── Reader Settings & Utilities ─────────────────────────────────

def render_reader_settings():
    """Reader Settings — 阅读设置"""
    img, draw = new_canvas()
    draw_header(draw, "阅读设置")
    y = HEADER_H + 14
    # Currently reading card
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+70), radius=8, fill=CLR_CARD)
    draw_sui_icon(img, "library", PAD+10, y+18, 34)
    draw.text((PAD+52, y+10), "三体", font=ui12(True), fill=CLR_FG)
    draw.text((PAD+52, y+36), "75% · 已读 245 页", font=small(), fill=CLR_SUBTITLE)
    draw_progress_ring(draw, W-PAD-30, y+35, 24, 75, th=5)
    y += 80
    items = [
        ("字号", "中"),
        ("行间距", "1.5倍"),
        ("字间距", "标准"),
        ("上边距", "12"),
        ("下边距", "12"),
        ("左边距", "8"),
        ("右边距", "8"),
        ("阅读背景", "纯白"),
        ("首行缩进", "已开启"),
        ("对齐方式", "左对齐"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 42 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+38), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), val, font=small(), fill=130)
        y += 42
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

def render_percent_selection():
    """Percent Selection — 百分比跳转"""
    img, draw = new_canvas()
    draw_header(draw, "跳转到...")
    y = HEADER_H + 40
    draw.text((PAD, y), "选择阅读进度", font=ui10(), fill=0)
    y += 40
    # 进度条
    draw_progress_bar(draw, 45, PAD, y, W-2*PAD, 12)
    y += 30
    draw.text((PAD, y), "45%", font=ui12(True), fill=0)
    y += 40
    # 常用百分比
    pcts = [10, 25, 50, 75, 90]
    for p in pcts:
        if y + 44 > H - FOOTER_H: break
        draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+38), radius=6, fill=243)
        draw.text((PAD, y+8), f"第 {p}%", font=ui10(), fill=0)
        y += 44
    draw_footer(draw, ["返回", "跳转", "上", "下"])
    return img

def render_font_selection():
    """Font Selection — 字体选择"""
    img, draw = new_canvas()
    draw_header(draw, "字体设置")
    y = HEADER_H + 14
    fonts = [
        ("默认字体", "思源黑体 · 系统"),
        ("宋体", "宋体 · 衬线"),
        ("楷体", "楷体 · 书法"),
        ("黑体", "黑体 · 无衬线"),
        ("圆体", "圆体 · 柔和"),
    ]
    for i, (name, desc) in enumerate(fonts):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), desc, font=small(), fill=90)
        y += 48
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img

def render_bookmark_manager():
    """Bookmark Manager — 书签管理"""
    img, draw = new_canvas()
    draw_header(draw, "书签管理")
    y = HEADER_H + 14
    bookmarks = [
        ("第一章 科学边界", "第8页 · 2026-08-20"),
        ("第三章 射手和农场主", "第32页 · 2026-08-21"),
        ("第五章 红岸基地", "第65页 · 2026-08-22"),
        ("第七章 三体游戏", "第98页 · 2026-08-23"),
    ]
    for i, (loc, date) in enumerate(bookmarks):
        if y + 50 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+46), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), loc, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), date, font=small(), fill=90)
        y += 50
    draw_footer(draw, ["返回", "跳转", "删除", "新增"])
    return img

def render_bookmark_notes():
    """Bookmark Notes — 书签笔记"""
    img, draw = new_canvas()
    draw_header(draw, "书签笔记")
    y = HEADER_H + 14
    notes = [
        ("宇宙就是一座黑暗森林", "三体 · 第45页"),
        ("人是为了活着本身而活着的", "活着 · 第32页"),
        ("道可道，非常道", "道德经 · 第1页"),
        ("天下皆知美之为美", "道德经 · 第3页"),
    ]
    for i, (quote, src) in enumerate(notes):
        if y + 54 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+50), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), f"「{quote}」", font=ui10(i==0), fill=0)
        draw.text((PAD, y+30), src, font=small(), fill=90)
        y += 54
    draw_footer(draw, ["返回", "编辑", "上", "下"])
    return img

def render_auto_page_turn():
    """Auto Page Turn Interval — 自动翻页间隔"""
    img, draw = new_canvas()
    draw_header(draw, "自动翻页间隔")
    y = HEADER_H + 14
    intervals = [
        ("5秒", "快速"),
        ("10秒", "推荐"),
        ("15秒", "适中"),
        ("30秒", "慢速"),
        ("60秒", "极慢"),
    ]
    for i, (label, desc) in enumerate(intervals):
        if y + 44 > H - FOOTER_H: break
        if i == 1:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+8), label, font=ui10(i==1), fill=0)
        draw.text((W-PAD-80, y+8), desc, font=small(), fill=90)
        y += 44
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img

def render_tilt_page_turn():
    """Tilt Page Turn — 倾斜翻页设置"""
    img, draw = new_canvas()
    draw_header(draw, "倾斜翻页")
    y = HEADER_H + 14
    items = [
        ("启用倾斜翻页", "已开启"),
        ("灵敏度", "中"),
        ("倾斜角度", "15°"),
        ("方向", "左倾/右倾"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=0)
            draw.text((PAD, y+8), name, font=ui10(True), fill=255)
            draw.text((W-PAD-60, y+8), val, font=small(True), fill=220)
        else:
            draw.text((PAD, y+8), name, font=ui10(), fill=0)
            draw.text((W-PAD-60, y+8), val, font=small(), fill=130)
        y += 48
    draw_footer(draw, ["返回", "切换", "上", "下"])
    return img


# ── Settings Screens ────────────────────────────────────────────

def render_settings_display():
    """Settings — Display tab"""
    img, draw = new_canvas()
    draw_header(draw, "系统设置")
    # Tab bar
    tabs = [("1)显示", True), ("2)按钮", False), ("3)系统", False)]
    draw_tab_bar(draw, tabs, HEADER_H + 6)
    y = HEADER_H + 48
    # Reading goal card
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+52), radius=8, fill=CLR_CARD)
    draw_progress_ring(draw, PAD+28, y+26, 18, 60, th=4)
    draw.text((PAD+56, y+8), "年度阅读目标", font=ui10(True), fill=CLR_FG)
    draw.text((PAD+56, y+30), "12 / 20 本 · 已读 3647 页", font=small(), fill=CLR_SUBTITLE)
    y += 60
    items = [
        ("锁屏壁纸", "透明叠加"),
        ("阅读进度", "无进度"),
        ("隐藏电池百分比", "从不"),
        ("刷新频率", "5页全刷"),
        ("永不全刷", "已关闭"),
        ("按钮提示", "已开启"),
        ("前光亮度", "60"),
        ("前光色温", "50"),
        ("关机前全刷", "已开启"),
        ("图片质量", "普通"),
        ("图标风格", "风格1"),
        ("图标选中风格", "仅圆角"),
        ("系统字号", "中"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 38 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+34), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+5), val, font=small(), fill=130)
        y += 38
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

def render_settings_controls():
    """Settings — Controls tab"""
    img, draw = new_canvas()
    draw_header(draw, "系统设置")
    tabs = [("1)显示", False), ("2)按钮", True), ("3)系统", False)]
    draw_tab_bar(draw, tabs, HEADER_H + 6)
    y = HEADER_H + 48
    # Reading goal card
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+52), radius=8, fill=CLR_CARD)
    draw_progress_ring(draw, PAD+28, y+26, 18, 60, th=4)
    draw.text((PAD+56, y+8), "\u5e74\u5ea6\u9605\u8bfb\u76ee\u6807", font=ui10(True), fill=CLR_FG)
    draw.text((PAD+56, y+30), "12 / 20 \u672c \u00b7 \u5df2\u8bfb 3647 \u9875", font=small(), fill=CLR_SUBTITLE)
    y += 60
    items = [
        ("映射侧边按钮", "已开启"),
        ("长按侧边按钮翻页", "已开启"),
        ("长按跳过章节", "已开启"),
        ("翻页键反转", "已关闭"),
        ("按键震动", "已关闭"),
        ("自定义按键映射", ""),
    ]
    for i, (name, val) in enumerate(items):
        if y + 38 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+34), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+5), val, font=small(), fill=130)
        y += 38
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

def render_settings_system():
    """Settings — System tab"""
    img, draw = new_canvas()
    draw_header(draw, "系统设置")
    tabs = [("1)显示", False), ("2)按钮", False), ("3)系统", True)]
    draw_tab_bar(draw, tabs, HEADER_H + 6)
    y = HEADER_H + 48
    # Reading goal card
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+52), radius=8, fill=CLR_CARD)
    draw_progress_ring(draw, PAD+28, y+26, 18, 60, th=4)
    draw.text((PAD+56, y+8), "\u5e74\u5ea6\u9605\u8bfb\u76ee\u6807", font=ui10(True), fill=CLR_FG)
    draw.text((PAD+56, y+30), "12 / 20 \u672c \u00b7 \u5df2\u8bfb 3647 \u9875", font=small(), fill=CLR_SUBTITLE)
    y += 60
    items = [
        ("系统语言", "简体中文"),
        ("开机自动同步时间", "已开启"),
        ("每次都重新选择WIFI", "已关闭"),
        ("直读TXT文档", "已开启"),
        ("休眠时间", "10分钟"),
        ("蓝牙设置", "已关闭"),
        ("坚果云配置", ""),
        ("数据胶囊配置", ""),
        ("SD卡升级", ""),
        ("还原为初始设置", ""),
        ("清理缓存", ""),
        ("切换启动区", "APP0（官方）"),
        ("开发者选项", ""),
        ("系统动画", "已开启"),
        ("动画步数", "8"),
        ("动画帧率", "中(0x44)"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 38 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+34), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+5), val, font=small(), fill=130)
        y += 38
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

def render_button_remap():
    """Button Remap — 按键映射"""
    img, draw = new_canvas()
    draw_header(draw, "按键映射")
    y = HEADER_H + 14
    items = [
        ("上键", "翻上页"),
        ("下键", "翻下页"),
        ("左键", "后退"),
        ("右键", "前进"),
        ("确认键", "确认"),
        ("返回键", "返回"),
        ("侧边键1", "目录"),
        ("侧边键2", "书签"),
    ]
    for i, (btn, action) in enumerate(items):
        if y + 40 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+36), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), btn, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), action, font=small(), fill=130)
        y += 40
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

def render_calibre_settings():
    """Calibre Settings"""
    img, draw = new_canvas()
    draw_header(draw, "Calibre 设置")
    y = HEADER_H + 14
    items = [
        ("Calibre 服务器", "192.168.1.100:8080"),
        ("用户名", "admin"),
        ("密码", "********"),
        ("自动连接", "已开启"),
        ("无线设备名", "Murphy M4"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "保存", "上", "下"])
    return img

def render_clear_cache():
    """Clear Cache"""
    img, draw = new_canvas()
    draw_header(draw, "清理缓存")
    y = HEADER_H + 20
    draw.text((PAD, y), "缓存占用", font=ui10(), fill=0)
    y += 30
    items = [
        ("EPUB 缓存", "128.5 MB"),
        ("封面缩略图", "45.2 MB"),
        ("阅读进度", "1.2 MB"),
        ("书签数据", "0.3 MB"),
        ("临时文件", "32.0 MB"),
    ]
    total = 0
    for i, (name, size) in enumerate(items):
        if y + 40 > H - FOOTER_H: break
        draw.text((PAD, y+5), name, font=ui10(), fill=0)
        draw.text((W-PAD-100, y+5), size, font=small(), fill=130)
        y += 40
        total += float(size.split()[0])
    y += 10
    draw.line((PAD-5, y, W-PAD+5, y), fill=0)
    y += 10
    draw.text((PAD, y), f"总计: {total} MB", font=ui12(True), fill=0)
    draw_footer(draw, ["返回", "全部清除", "上", "下"])
    return img

def render_data_capsule_settings():
    """Data Capsule Settings"""
    img, draw = new_canvas()
    draw_header(draw, "数据胶囊配置")
    y = HEADER_H + 14
    items = [
        ("服务器地址", "dav.example.com"),
        ("用户名", "user@example.com"),
        ("密码", "********"),
        ("同步目录", "/Books/"),
        ("自动同步", "已开启"),
        ("同步间隔", "30分钟"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "保存", "上", "下"])
    return img


def render_developer_options():
    """Developer Options"""
    img, draw = new_canvas()
    draw_header(draw, "开发者选项")
    y = HEADER_H + 14
    items = [
        ("USB 调试", "已开启"),
        ("WiFi 调试", "已关闭"),
        ("日志输出级别", "信息"),
        ("屏幕刷新模式", "快速"),
        ("显示帧率", "已关闭"),
        ("强制全刷", ""),
        ("进入 DFU 模式", ""),
        ("运行测试", ""),
    ]
    for i, (name, val) in enumerate(items):
        if y + 40 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+36), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), val, font=small(), fill=130)
        y += 40
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img

def render_jianguo_yun_settings():
    """JianGuoYun Settings"""
    img, draw = new_canvas()
    draw_header(draw, "坚果云配置")
    y = HEADER_H + 14
    items = [
        ("坚果云账号", "user@example.com"),
        ("应用密码", "********"),
        ("同步文件夹", "/我的书籍"),
        ("自动同步", "已开启"),
        ("仅WiFi同步", "已开启"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "保存", "上", "下"])
    return img

def render_koreader_auth():
    """KOReader Auth"""
    img, draw = new_canvas()
    draw_header(draw, "KOReader 登录")
    y = HEADER_H + 14
    draw.text((PAD, y), "请登录 KOReader 账号以同步阅读数据", font=small(), fill=90)
    y += 36
    items = [
        ("用户名", "reader@example.com"),
        ("密码", "********"),
        ("服务器", "https://sync.koreader.com"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        if i < 2:
            draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        else:
            draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "登录", "上", "下"])
    return img

def render_koreader_settings():
    """KOReader Settings"""
    img, draw = new_canvas()
    draw_header(draw, "KOReader 同步")
    y = HEADER_H + 14
    items = [
        ("自动同步", "已开启"),
        ("同步阅读进度", "已开启"),
        ("同步书签", "已开启"),
        ("文档匹配方式", "文件名"),
        ("最后同步", "2026-08-27 10:30"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-120, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "同步", "上", "下"])
    return img

def render_number_selection():
    """Number Selection"""
    img, draw = new_canvas()
    draw_header(draw, "数值选择")
    y = HEADER_H + 40
    draw.text((PAD, y), "选择数值", font=ui10(), fill=0)
    y += 40
    val = 12
    draw.rounded_rectangle((W//2-40, y, W//2+40, y+50), radius=8, fill=CLR_SELECTED)
    draw.text((W//2-15, y+10), str(val), font=ui12(True), fill=0)
    y += 70
    draw.text((PAD, y), "最小值: 1    最大值: 30    步长: 1", font=small(), fill=90)
    y += 40
    draw.rounded_rectangle((PAD, y, W//2-10, y+40), radius=6, fill=243)
    draw.text((PAD+20, y+8), "-", font=ui12(True), fill=0)
    draw.rounded_rectangle((W//2+10, y, W-PAD, y+40), radius=6, fill=243)
    draw.text((W//2+50, y+8), "+", font=ui12(True), fill=0)
    draw_footer(draw, ["返回", "确认", "", ""])
    return img

def render_online_ota():
    """Online OTA"""
    img, draw = new_canvas()
    draw_header(draw, "在线更新")
    y = HEADER_H + 20
    draw.text((PAD, y), "当前版本: v2.1.0 (build 20260801)", font=ui10(), fill=0)
    y += 40
    draw.text((PAD, y), "最新版本: v2.1.2 (build 20260820)", font=ui10(True), fill=0)
    y += 40
    draw.text((PAD, y), "更新内容:", font=ui10(), fill=0)
    y += 30
    changelog = [
        "• 修复部分EPUB图片显示问题",
        "• 优化WiFi连接稳定性",
        "• 新增自动翻页功能",
        "• 改进电池续航管理",
    ]
    for line in changelog:
        draw.text((PAD, y), line, font=small(), fill=90)
        y += 24
    y += 20
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-40, y+10), "检查更新", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "更新", "", ""])
    return img

def render_ota_update():
    """OTA Update — 升级进度"""
    img, draw = new_canvas()
    draw_header(draw, "系统升级")
    y = HEADER_H + 60
    draw.text((PAD, y), "正在下载更新...", font=ui10(), fill=0)
    y += 50
    draw_progress_bar(draw, 67, PAD, y, W-2*PAD, 14)
    y += 30
    draw.text((PAD, y), "67%", font=ui12(True), fill=0)
    y += 30
    draw.text((PAD, y), "请勿关闭设备", font=small(), fill=130)
    y += 30
    draw.text((PAD, y), "正在下载: update-v2.1.2.bin (4.2MB/6.3MB)", font=small(), fill=90)
    draw_footer(draw, ["", "", "", ""])
    return img

def render_reset_settings():
    """Reset Settings"""
    img, draw = new_canvas()
    draw_header(draw, "还原初始设置")
    y = HEADER_H + 30
    draw.text((PAD, y), "此操作将还原所有设置为出厂状态。", font=ui10(), fill=0)
    y += 30
    draw.text((PAD, y), "以下数据将被清除:", font=ui10(True), fill=0)
    y += 30
    items = [
        "• 所有系统设置",
        "• WiFi 密码",
        "• 蓝牙配对信息",
        "• 云服务账号",
        "• 阅读偏好设置",
    ]
    for item in items:
        draw.text((PAD, y), item, font=small(), fill=90)
        y += 24
    y += 20
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-40, y+10), "还原设置", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "确认", "", ""])
    return img

def render_simple_bluetooth():
    """Bluetooth Settings"""
    img, draw = new_canvas()
    draw_header(draw, "蓝牙设置")
    y = HEADER_H + 14
    draw.text((PAD, y), "蓝牙", font=ui10(True), fill=0)
    y += 8
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
    draw.text((PAD, y+6), "启用蓝牙", font=ui10(True), fill=0)
    draw.rounded_rectangle((W-76, y+6, W-30, y+34), radius=14, outline=0, width=1, fill=215)
    draw.text((W-70, y+11), "开", font=small(True), fill=0)
    y += 52
    draw.line((PAD, y, W-PAD, y), fill=0)
    y += 14
    draw.text((PAD, y), "已配对设备", font=ui10(True), fill=0)
    y += 32
    devices = [
        ("蓝牙键盘", "已连接"),
        ("蓝牙耳机", "未连接"),
        ("手机 - Murphy", "已配对"),
    ]
    for i, (name, state) in enumerate(devices):
        if y + 44 > H - FOOTER_H: break
        draw.text((PAD, y+5), name, font=ui10(), fill=0)
        draw.text((W-PAD-80, y+5), state, font=small(), fill=90)
        y += 44
    draw_footer(draw, ["返回", "扫描", "上", "下"])
    return img


# ── App Screens ─────────────────────────────────────────────────

def render_app_list():
    """App List — 应用列表"""
    img, draw = new_canvas()
    draw_header(draw, "应用")
    y = HEADER_H + 14
    apps = [
        ("KOReader 同步", "数据同步"),
        ("晋江云同步", "云存储"),
        ("Calibre 传输", "无线传书"),
        ("OPDS 浏览器", "在线书库"),
        ("数据胶囊", "WebDAV"),
        ("屏幕桥接", "远程控制"),
        ("文件管理器", "系统工具"),
    ]
    for i, (name, desc) in enumerate(apps):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), desc, font=small(), fill=90)
        draw.text((W-PAD-40, y+28), "✓", font=small(), fill=130)
        y += 48
    draw_footer(draw, ["返回", "卸载", "上", "下"])
    return img

def render_app_install():
    """App Install — 安装扩展"""
    img, draw = new_canvas()
    draw_header(draw, "安装扩展")
    y = HEADER_H + 14
    items = [
        ("📦 ko-sync-v2.mpk", "KOReader 同步  v2.0"),
        ("📦 jianguo.mpk", "晋江云同步  v1.3"),
        ("📦 calibre-wireless.mpk", "Calibre 无线传书  v1.1"),
        ("📦 opds-browser.mpk", "OPDS 浏览器  v2.0"),
    ]
    for i, (name, desc) in enumerate(items):
        if y + 52 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+48), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+30), desc, font=small(), fill=90)
        draw.text((W-PAD-50, y+30), "安装", font=small(True), fill=0)
        y += 52
    draw_footer(draw, ["返回", "安装", "上", "下"])
    return img

def render_app_runtime():
    """App Runtime — 应用运行时"""
    img, draw = new_canvas()
    draw_header(draw, "应用运行时")
    y = HEADER_H + 14
    draw.text((PAD, y), "正在运行的应用", font=ui10(True), fill=0)
    y += 36
    items = [
        ("KOReader 同步", "后台运行中"),
        ("晋江云同步", "空闲"),
    ]
    for i, (name, state) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), state, font=small(), fill=90)
        y += 44
    y += 10
    draw.line((PAD, y, W-PAD, y), fill=0)
    y += 14
    draw.text((PAD, y), "内存使用: 12.5 MB / 64 MB", font=small(), fill=90)
    draw_footer(draw, ["返回", "停止", "上", "下"])
    return img

def render_native_app():
    """Native App — 原生应用"""
    img, draw = new_canvas()
    draw_header(draw, "原生应用")
    y = HEADER_H + 14
    apps = [
        ("📖 EPUB 阅读器", "版本 2.1.0"),
        ("📖 XTC 阅读器", "版本 2.1.0"),
        ("📖 TXT 阅读器", "版本 2.1.0"),
        ("📚 我的书架", "版本 2.1.0"),
        ("⚙ 系统设置", "版本 2.1.0"),
    ]
    for i, (name, ver) in enumerate(apps):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+5), ver, font=small(), fill=130)
        y += 48
    draw_footer(draw, ["返回", "打开", "上", "下"])
    return img

def render_native_provider_book():
    """Native Provider — Book"""
    img, draw = new_canvas()
    draw_header(draw, "内容提供商")
    y = HEADER_H + 14
    providers = [
        ("本地文件", "SD 卡书籍"),
        ("KOReader", "同步书籍"),
        ("晋江云", "云存储书籍"),
        ("Calibre", "无线传书"),
        ("OPDS", "在线书库"),
        ("数据胶囊", "WebDAV 书籍"),
    ]
    for i, (name, desc) in enumerate(providers):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), desc, font=small(), fill=90)
        y += 48
    draw_footer(draw, ["返回", "选择", "上", "下"])
    return img

def render_native_provider_login():
    """Native Provider — Login"""
    img, draw = new_canvas()
    draw_header(draw, "登录")
    y = HEADER_H + 30
    draw.text((PAD, y), "请登录以访问内容提供商", font=ui10(), fill=0)
    y += 50
    items = [
        ("服务器地址", "example.com"),
        ("用户名", "user"),
        ("密码", "********"),
    ]
    for i, (name, val) in enumerate(items):
        draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+40), radius=6, fill=243)
        draw.text((PAD, y+8), name, font=ui10(), fill=0)
        draw.text((W-PAD-120, y+8), val, font=small(), fill=130)
        draw.line((PAD, y+40, W-PAD, y+40), fill=235)
        y += 44
    y += 10
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-20, y+10), "登录", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "登录", "", ""])
    return img

def render_native_provider_endpoint():
    """Native Provider — Endpoint"""
    img, draw = new_canvas()
    draw_header(draw, "端点配置")
    y = HEADER_H + 14
    items = [
        ("端点 URL", "https://api.example.com/v1"),
        ("API Key", "sk-xxxxxxxxxxxx"),
        ("超时时间", "30秒"),
        ("重试次数", "3"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-140, y+6), val, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "测试", "上", "下"])
    return img

def render_screen_bridge():
    """Screen Bridge — 屏幕桥接"""
    img, draw = new_canvas()
    draw_header(draw, "屏幕桥接")
    y = HEADER_H + 20
    draw.text((PAD, y), "将设备屏幕投射到浏览器", font=ui10(), fill=0)
    y += 40
    draw.text((PAD, y), "状态:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "运行中", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "地址:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "http://192.168.1.100:8080", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "连接设备:", font=ui10(True), fill=0)
    draw.text((PAD+100, y), "1", font=ui10(), fill=0)
    y += 50
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-40, y+10), "停止桥接", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "刷新", "", ""])
    return img


# ── Network Screens ─────────────────────────────────────────────

def render_wifi_selection():
    """WiFi Selection"""
    img, draw = new_canvas()
    draw_header(draw, "Wi-Fi 设置")
    y = HEADER_H + 14
    draw.text((PAD, y), "Wi-Fi", font=ui10(True), fill=0)
    y += 8
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
    draw.text((PAD, y+6), "启用 Wi-Fi", font=ui10(True), fill=0)
    draw.rounded_rectangle((W-76, y+6, W-30, y+34), radius=14, outline=0, width=1, fill=215)
    draw.text((W-70, y+11), "开", font=small(True), fill=0)
    y += 52
    draw.line((PAD, y, W-PAD, y), fill=0)
    y += 14
    draw.text((PAD, y), "可用网络", font=ui10(True), fill=0)
    y += 32
    nets = [
        ("MurphyM4-5G", "已连接 · 信号良好"),
        ("HomeNet", "加密"),
        ("TP-Link_24G", "信号弱"),
        ("Starbucks_WiFi", "需登录"),
        ("其他网络…", "手动输入 SSID"),
    ]
    for i, (name, state) in enumerate(nets):
        if y + 48 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+44), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), state, font=small(), fill=90)
        y += 48
    draw_footer(draw, ["返回", "连接", "刷新"])
    return img

def render_network_mode():
    """Network Mode Selection"""
    img, draw = new_canvas()
    draw_header(draw, "网络模式")
    y = HEADER_H + 14
    draw.text((PAD, y), "你想如何连接?", font=ui10(True), fill=0)
    y += 36
    modes = [
        ("1) 手机连接到设备传书（推荐）", "通过设备热点直连传书"),
        ("2) 设备连接到WiFi传书", "设备连接路由器后传书"),
        ("3) 使用Calibre无线设备传输", "通过Calibre无线传书"),
    ]
    for i, (name, desc) in enumerate(modes):
        if y + 52 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+48), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((PAD, y+28), desc, font=small(), fill=90)
        y += 52
    draw_footer(draw, ["返回", "继续", "上", "下"])
    return img

def render_calibre_connect():
    """Calibre Connect"""
    img, draw = new_canvas()
    draw_header(draw, "Calibre 无线传书")
    y = HEADER_H + 20
    draw.text((PAD, y), "通过 Calibre 无线传输书籍", font=ui10(), fill=0)
    y += 40
    draw.text((PAD, y), "状态:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "等待连接", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "服务器地址:", font=ui10(True), fill=0)
    draw.text((PAD+100, y), "192.168.1.100:8080", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "设备名称:", font=ui10(True), fill=0)
    draw.text((PAD+100, y), "Murphy M4", font=ui10(), fill=0)
    y += 50
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-40, y+10), "开始连接", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "刷新", "", ""])
    return img

def render_cross_point_web_server():
    """CrossPoint Web Server"""
    img, draw = new_canvas()
    draw_header(draw, "Web 服务器")
    y = HEADER_H + 20
    draw.text((PAD, y), "通过浏览器管理设备", font=ui10(), fill=0)
    y += 40
    draw.text((PAD, y), "状态:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "运行中", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "地址:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "http://192.168.1.100", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "端口:", font=ui10(True), fill=0)
    draw.text((PAD+60, y), "80", font=ui10(), fill=0)
    y += 50
    draw.rounded_rectangle((PAD, y, W-PAD, y+44), radius=6, fill=0)
    draw.text((W//2-40, y+10), "停止服务器", font=ui12(True), fill=255)
    draw_footer(draw, ["返回", "打开", "", ""])
    return img

# ── Browser Screens ─────────────────────────────────────────────

def render_opds_browser():
    """OPDS Book Browser"""
    img, draw = new_canvas()
    draw_header(draw, "OPDS 书库")
    y = HEADER_H + 14
    draw.text((PAD, y), "服务器", font=ui10(True), fill=0)
    y += 8
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+36), radius=6, fill=243)
    draw.text((PAD, y+6), "https://opds.example.com/opds", font=small(), fill=0)
    y += 48
    categories = [
        ("📚 最新上架", "120 本"),
        ("⭐ 热门推荐", "85 本"),
        ("🏆 经典文学", "230 本"),
        ("🔬 科学技术", "156 本"),
        ("💭 哲学思想", "78 本"),
        ("📖 历史传记", "92 本"),
    ]
    for i, (cat, count) in enumerate(categories):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), cat, font=ui10(i==0), fill=0)
        draw.text((W-PAD-60, y+5), count, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "浏览", "上", "下"])
    return img

def render_jianguo_browser():
    """JianGuo Browser"""
    img, draw = new_canvas()
    draw_header(draw, "坚果云")
    y = HEADER_H + 14
    draw.text((PAD, y), "我的文件", font=ui10(True), fill=0)
    y += 36
    files = [
        ("📁 小说/", "12 本"),
        ("📁 历史/", "5 本"),
        ("📁 科技/", "8 本"),
        ("📖 三体.epub", "28.5MB"),
        ("📖 活着.txt", "512KB"),
        ("📖 百年孤独.epub", "18.2MB"),
        ("📖 人类简史.xtc", "24.1MB"),
    ]
    for i, (name, size) in enumerate(files):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-60, y+5), size, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "下载", "上", "下"])
    return img

def render_data_capsule_browser():
    """Data Capsule Browser"""
    img, draw = new_canvas()
    draw_header(draw, "数据胶囊")
    y = HEADER_H + 14
    draw.text((PAD, y), "我的文件", font=ui10(True), fill=0)
    y += 36
    files = [
        ("📁 Documents/", "15 个文件"),
        ("📁 Books/", "23 个文件"),
        ("📖 report.pdf", "2.5MB"),
        ("📖 notes.txt", "128KB"),
        ("📖 data.json", "512KB"),
    ]
    for i, (name, size) in enumerate(files):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+5), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+5), size, font=small(), fill=130)
        y += 44
    draw_footer(draw, ["返回", "下载", "上", "下"])
    return img


# ── Boot / Sleep / Status Screens ───────────────────────────────

def render_boot():
    """Boot Screen"""
    img, draw = new_canvas()
    # 全黑背景
    draw.rectangle((0, 0, W, H), fill=0)
    draw.text((W//2-60, H//2-20), "Murphy M4", font=face(36, True), fill=255)
    draw.text((W//2-50, H//2+30), "正在启动...", font=ui12(), fill=200)
    draw.text((W//2-80, H//2+70), "v2.1.0 (build 20260801)", font=small(), fill=150)
    return img

def render_sleep():
    """Sleep Screen"""
    img, draw = new_canvas()
    draw.rectangle((0, 0, W, H), fill=0)
    draw.text((W//2-40, H//2-15), "已休眠", font=face(36, True), fill=255)
    draw.text((W//2-60, H//2+30), "轻触唤醒", font=ui12(), fill=200)
    return img

def render_full_screen_message():
    """Full Screen Message"""
    img, draw = new_canvas()
    draw.rectangle((0, 0, W, H), fill=255)
    draw.text((W//2-80, H//2-30), "消息", font=face(36, True), fill=0)
    draw.text((W//2-100, H//2+20), "操作已完成", font=ui12(), fill=0)
    draw.text((W//2-80, H//2+55), "长按返回", font=small(), fill=130)
    return img

def render_home_mem_warning():
    """Home Memory Warning — 低内存提示"""
    img, draw = new_canvas()
    draw.rectangle((0, 0, W, H), fill=255)
    # 对话框
    box_x, box_y = 80, 280
    box_w, box_h = W - 160, 200
    draw.rounded_rectangle((box_x, box_y, box_x+box_w, box_y+box_h), radius=10, fill=240)
    draw.rounded_rectangle((box_x-3, box_y-3, box_x+box_w+3, box_y+box_h+3), radius=10, fill=0, outline=0, width=2)
    draw.text((W//2-80, box_y+20), "内存不足", font=ui12(True), fill=0)
    draw.text((W//2-100, box_y+60), "请关闭部分应用后重试", font=ui10(), fill=0)
    draw.text((W//2-80, box_y+95), "或重启设备", font=ui10(), fill=0)
    # 按钮
    draw.rounded_rectangle((box_x+20, box_y+140, box_x+80, box_y+175), radius=6, fill=0)
    draw.text((box_x+30, box_y+148), "取消", font=ui10(True), fill=255)
    draw.rounded_rectangle((box_x+120, box_y+140, box_x+200, box_y+175), radius=6, fill=255)
    draw.text((box_x+130, box_y+148), "重启", font=ui10(True), fill=0)
    return img

# ── Input Utility Screens ───────────────────────────────────────

def render_keyboard_entry():
    """Keyboard Entry — 键盘输入"""
    img, draw = new_canvas()
    draw_header(draw, "输入")
    y = HEADER_H + 20
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+44), radius=6, fill=243)
    draw.text((PAD, y+10), "搜索书籍...", font=ui10(), fill=130)
    y += 60
    # 模拟键盘布局
    keyboard = [
        "q w e r t y u i o p",
        "a s d f g h j k l",
        "z x c v b n m 删除",
        "123  空格  确认",
    ]
    kx = 30
    for row in keyboard:
        for ch in row.split(" "):
            if ch:
                draw.text((kx, y), ch, font=ui10(), fill=0)
                kx += 48
            else:
                kx += 24
        kx = 30
        y += 36
    draw_footer(draw, ["返回", "输入", "", ""])
    return img

# ── Sync / Misc Screens ─────────────────────────────────────────

def render_jianguo_sync():
    """JianGuo Sync"""
    img, draw = new_canvas()
    draw_header(draw, "晋江云同步")
    y = HEADER_H + 20
    draw.text((PAD, y), "同步状态", font=ui10(True), fill=0)
    y += 36
    draw.text((PAD, y), "最后同步: 2026-08-27 10:30", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "同步中...", font=ui10(), fill=0)
    y += 10
    draw_progress_bar(draw, 55, PAD, y, W-2*PAD, 8)
    y += 24
    draw.text((PAD, y), "正在同步: 三体.epub (12.5MB/28.5MB)", font=small(), fill=90)
    y += 40
    draw.text((PAD, y), "同步队列:", font=ui10(True), fill=0)
    y += 30
    items = ["三体.epub", "活着.txt", "百年孤独.epub", "人类简史.xtc"]
    for i, item in enumerate(items):
        draw.text((PAD, y), f"{i+1}. {item}", font=small(), fill=90)
        y += 22
    draw_footer(draw, ["返回", "暂停", "", ""])
    return img

def render_koreader_sync():
    """KOReader Sync"""
    img, draw = new_canvas()
    draw_header(draw, "KOReader 同步")
    y = HEADER_H + 20
    draw.text((PAD, y), "同步状态", font=ui10(True), fill=0)
    y += 36
    draw.text((PAD, y), "已连接: reader@example.com", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "最后同步: 2026-08-27 10:30", font=ui10(), fill=0)
    y += 36
    draw.text((PAD, y), "同步进度:", font=ui10(), fill=0)
    y += 10
    draw_progress_bar(draw, 100, PAD, y, W-2*PAD, 8)
    y += 30
    draw.text((PAD, y), "✓ 阅读进度已同步", font=small(), fill=90)
    y += 24
    draw.text((PAD, y), "✓ 书签已同步", font=small(), fill=90)
    y += 24
    draw.text((PAD, y), "✓ 阅读设置已同步", font=small(), fill=90)
    draw_footer(draw, ["返回", "立即同步", "", ""])
    return img

# ── Reader Settings (EpubReaderSettings) ────────────────────────

def render_epub_reader_settings():
    """Epub Reader Settings"""
    img, draw = new_canvas()
    draw_header(draw, "EPUB 阅读设置")
    y = HEADER_H + 14
    # Reading goal card
    draw.rounded_rectangle((PAD-5, y, W-PAD+5, y+52), radius=8, fill=CLR_CARD)
    draw_progress_ring(draw, PAD+28, y+26, 18, 60, th=4)
    draw.text((PAD+56, y+8), "年度阅读目标", font=ui10(True), fill=CLR_FG)
    draw.text((PAD+56, y+30), "12 / 20 本 · 已读 3647 页", font=small(), fill=CLR_SUBTITLE)
    y += 60
    items = [
        ("显示时间", "已开启"),
        ("显示EPUB图片", "已开启"),
        ("标点宽度", "标准"),
        ("下划线", "已关闭"),
        ("段落间距", "标准"),
        ("侧边按钮设置", "仅阅读"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 42 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+38), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), val, font=small(), fill=130)
        y += 42
    draw_footer(draw, ["返回", "修改", "上", "下"])
    return img

# ── ActivityWithSubactivity ─────────────────────────────────────

def render_activity_with_sub():
    """Activity With Subactivity"""
    img, draw = new_canvas()
    draw_header(draw, "子页面")
    y = HEADER_H + 14
    items = [
        "子页面 1",
        "子页面 2",
        "子页面 3",
        "子页面 4",
        "返回主页面",
    ]
    for i, item in enumerate(items):
        if y + 44 > H - FOOTER_H: break
        if i == 0:
            draw.rounded_rectangle((PAD-5, y-2, W-PAD+5, y+40), radius=6, fill=CLR_SELECTED)
        draw.text((PAD, y+8), item, font=ui10(i==0), fill=0)
        y += 44
    draw_footer(draw, ["返回", "确认", "上", "下"])
    return img


# ── SCREENS 注册表 ──────────────────────────────────────────────
# 所有屏幕渲染函数注册在此，顺序决定 --all 时的输出顺序

SCREENS = [
    # Home & Navigation
    ("home", render_home, "主页"),
    ("home_mem_warning", render_home_mem_warning, "内存不足"),
    ("my_library", render_my_library, "我的书架"),
    ("recent_books", render_recent_books, "最近阅读"),
    # Reader pages
    ("reader_epub", render_reader_epub, "EPUB 阅读"),
    ("reader_xtc", render_reader_xtc, "XTC 阅读"),
    ("reader_txt", render_reader_txt, "TXT 阅读"),
    # Reader menus
    ("reader_menu_epub", render_reader_menu_epub, "EPUB 菜单"),
    ("reader_menu_xtc", render_reader_menu_xtc, "XTC 菜单"),
    ("chapter_epub", render_chapter_epub, "EPUB 目录"),
    ("chapter_xtc", render_chapter_xtc, "XTC 目录"),
    ("chapter_txt", render_chapter_txt, "TXT 目录"),
    ("reader_settings", render_reader_settings, "阅读设置"),
    ("percent_selection", render_percent_selection, "跳转选择"),
    # Reader features
    ("font_selection", render_font_selection, "字体选择"),
    ("bookmark_manager", render_bookmark_manager, "书签管理"),
    ("bookmark_notes", render_bookmark_notes, "书签笔记"),
    ("auto_page_turn", render_auto_page_turn, "自动翻页"),
    ("tilt_page_turn", render_tilt_page_turn, "倾斜翻页"),
    # Settings
    ("settings_display", render_settings_display, "设置-显示"),
    ("settings_controls", render_settings_controls, "设置-按钮"),
    ("settings_system", render_settings_system, "设置-系统"),
    ("button_remap", render_button_remap, "按键映射"),
    ("calibre_settings", render_calibre_settings, "Calibre 设置"),
    ("clear_cache", render_clear_cache, "清理缓存"),
    ("data_capsule_settings", render_data_capsule_settings, "数据胶囊配置"),
    ("developer_options", render_developer_options, "开发者选项"),
    ("jianguo_yun_settings", render_jianguo_yun_settings, "坚果云配置"),
    ("koreader_auth", render_koreader_auth, "KOReader 登录"),
    ("koreader_settings", render_koreader_settings, "KOReader 同步"),
    ("number_selection", render_number_selection, "数值选择"),
    ("online_ota", render_online_ota, "在线更新"),
    ("ota_update", render_ota_update, "系统升级"),
    ("reset_settings", render_reset_settings, "还原设置"),
    ("simple_bluetooth", render_simple_bluetooth, "蓝牙设置"),
    # Apps
    ("app_list", render_app_list, "应用列表"),
    ("app_install", render_app_install, "安装扩展"),
    ("app_runtime", render_app_runtime, "应用运行时"),
    ("native_app", render_native_app, "原生应用"),
    ("native_provider_book", render_native_provider_book, "内容提供商"),
    ("native_provider_login", render_native_provider_login, "登录"),
    ("native_provider_endpoint", render_native_provider_endpoint, "端点配置"),
    ("screen_bridge", render_screen_bridge, "屏幕桥接"),
    # Network
    ("wifi_selection", render_wifi_selection, "Wi-Fi"),
    ("network_mode", render_network_mode, "网络模式"),
    ("calibre_connect", render_calibre_connect, "Calibre 连接"),
    ("cross_point_web_server", render_cross_point_web_server, "Web 服务器"),
    # Browser
    ("opds_browser", render_opds_browser, "OPDS 书库"),
    ("jianguo_browser", render_jianguo_browser, "坚果云浏览"),
    ("data_capsule_browser", render_data_capsule_browser, "数据胶囊浏览"),
    # Boot / Sleep / Status
    ("boot", render_boot, "开机"),
    ("sleep", render_sleep, "休眠"),
    ("full_screen_message", render_full_screen_message, "全屏消息"),
    # Sync
    ("jianguo_sync", render_jianguo_sync, "晋江云同步"),
    ("koreader_sync", render_koreader_sync, "KOReader 同步"),
    # Reader Settings
    ("epub_reader_settings", render_epub_reader_settings, "EPUB 阅读设置"),
    # Utility
    ("keyboard_entry", render_keyboard_entry, "键盘输入"),
    ("activity_with_sub", render_activity_with_sub, "子页面"),
]

SCREENS_BY_NAME = {name: (fn, label) for name, fn, label in SCREENS}


# 外部静态图资产：这些屏直接拷贝工程内提供的 PNG，而非用 PIL 重新渲染。
# 生成时优先使用该资产，避免被 render_* 覆盖；资产本身在 git 中可跟踪。
_TOOL_DIR = Path(__file__).resolve().parent
CUSTOM_ASSETS = {
    "home": _TOOL_DIR / "preview_assets" / "home.png",  # Nothing 点阵风主页 (400×800)
}



# ── 画廊生成 ────────────────────────────────────────────────────

def _write_gallery(out: Path) -> None:
    """生成 index.html 画廊"""
    existing = [(name, label) for name, _, label in SCREENS
                if (out / f"{name}.png").exists()]
    if not existing:
        return

    def card(name: str, label: str) -> str:
        return (
            f'<div class="c">'
            f'<h3>{label}</h3>'
            f'<p class="n">{name}.png</p>'
            f'<img src="{name}.png" alt="{label}">'
            f'</div>'
        )

    html = (
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>Murphy M4 全 UI 预览</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#f4f4f4;margin:0;padding:24px}"
        "h1{font-size:22px;margin-bottom:6px}"
        "p.d{color:#666;font-size:13px;margin:0 0 18px}"
        ".wrap{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:14px}"
        ".c{background:#fff;border:1px solid #ddd;border-radius:10px;padding:10px;box-shadow:0 1px 3px rgba(0,0,0,.08)}"
        "h3{margin:0 0 2px;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        ".n{color:#999;font-size:11px;margin:0 0 8px}"
        "img{width:100%;height:auto;border:1px solid #eee;image-rendering:pixelated}"
        "</style></head><body>"
        f"<h1>Murphy M4 全 UI 预览</h1>"
        f"<p class='d'>共 {len(existing)} 个屏幕 · 480×800</p>"
        "<div class='wrap'>"
    )
    html += "".join(card(n, l) for n, l in existing)
    html += "</div></body></html>"
    (out / "index.html").write_text(html, encoding="utf-8")


def render_all(out: Path, screens: list[str]) -> list[Path]:
    """渲染指定屏幕列表"""
    out.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    for name in screens:
        if name not in SCREENS_BY_NAME:
            print(f"  ⚠ 跳过未知屏幕: {name}")
            continue
        fn, label = SCREENS_BY_NAME[name]
        path = out / f"{name}.png"
        asset = CUSTOM_ASSETS.get(name)
        if asset is not None and asset.exists():
            shutil.copy2(asset, path)
            print(f"  ✓ {label} ({name}.png · 外部资产)")
        else:
            img = fn()
            img.save(path)
            print(f"  ✓ {label} ({name}.png)")
        paths.append(path)
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description="Murphy M4 全 UI 预览台")
    choices = [name for name, _, _ in SCREENS] + ["all"]
    parser.add_argument("--screen", choices=choices, default="all",
                        help="渲染哪个屏幕 (默认: all)")
    parser.add_argument("--out", type=Path, default=Path("build/m4ui-preview"))
    parser.add_argument("--watch", action="store_true",
                        help="持续重建")
    parser.add_argument("--font", type=Path, default=None,
                        help="覆盖 CJK 常规字体")
    parser.add_argument("--bold", type=Path, default=None,
                        help="覆盖 CJK 粗体字体")
    args = parser.parse_args()

    global REGULAR, BOLD
    if args.font is not None:
        if not args.font.exists():
            raise SystemExit(f"字体文件未找到: {args.font}")
        REGULAR = str(args.font)
    if args.bold is not None:
        if not args.bold.exists():
            raise SystemExit(f"粗体文件未找到: {args.bold}")
        BOLD = str(args.bold)

    if args.screen == "all":
        screen_list = [name for name, _, _ in SCREENS]
    else:
        screen_list = [args.screen]

    while True:
        paths = render_all(args.out, screen_list)
        _write_gallery(args.out)
        print(f"\n  → {len(paths)} 个 PNG 已生成到 {args.out.resolve()}")
        print(f"  → 画廊: {args.out / 'index.html'}")
        if not args.watch:
            break
        time.sleep(0.5)


if __name__ == "__main__":
    main()
