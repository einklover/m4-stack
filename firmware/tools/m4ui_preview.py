#!/usr/bin/env python3
"""Murphy M4 480x800 UI preview workbench.

The preview mirrors the fixed runtime-TTF chrome policy used on device:

  SMALL = 18px
  UI_10 = 22px
  UI_12 = 26px

System UI and Native plugins reuse these exact raster sizes; Reader body size is
independent. Category navigation is a flat 4x2 text grid with only the selected
item receiving a subtle gray pill. Secondary metadata is rendered gray here to
approximate the firmware's Bayer-dithered 1-bit treatment.

Examples:
  python tools/m4ui_preview.py                 # render every screen
  python tools/m4ui_preview.py --screen both   # home + detail (legacy)
  python tools/m4ui_preview.py --screen reader --watch
  python tools/m4ui_preview.py --font C:/Windows/Fonts/simsun.ttc
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 800
PAD = 30
TILE_PAD = 20
HEADER_H = 50
FOOTER_H = 46
GAP = 6
TILE_BLOCK_H = 116

# Must match src/util/M4RuntimeUiFontPolicy.h.
SMALL_PX = 18
UI_10_PX = 22
UI_12_PX = 26
# Reader body is independent of the fixed system chrome (mirrors the firmware
# runtime-TTF reader faces); kept slightly larger here to show page layout.
READER_BODY_PX = 28

FONT_CANDIDATES = [
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/STZHONGS.TTF",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
]
BOLD_CANDIDATES = [
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/Dengb.ttf",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/STZHONGS.TTF",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
]


def first_existing(paths: list[str]) -> str:
    for p in paths:
        if Path(p).exists():
            return p
    raise SystemExit("No CJK font found. Install Noto Sans CJK in the preview environment.")


REGULAR = first_existing(FONT_CANDIDATES)
BOLD = first_existing(BOLD_CANDIDATES)


def face(px: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(BOLD if bold else REGULAR, px)


def ui10(bold: bool = False) -> ImageFont.FreeTypeFont:
    return face(UI_10_PX, bold)


def ui12(bold: bool = False) -> ImageFont.FreeTypeFont:
    return face(UI_12_PX, bold)


def small(bold: bool = False) -> ImageFont.FreeTypeFont:
    return face(SMALL_PX, bold)


def text_width(draw: ImageDraw.ImageDraw, text: str, f: ImageFont.FreeTypeFont) -> int:
    return int(draw.textlength(text, font=f))


def fit(draw: ImageDraw.ImageDraw, text: str, f: ImageFont.FreeTypeFont, width: int) -> str:
    if text_width(draw, text, f) <= width:
        return text
    ellipsis = "..."
    while text and text_width(draw, text + ellipsis, f) > width:
        text = text[:-1]
    return text + ellipsis


def normalize_breaks(text: str) -> str:
    for tag in ("<br/>", "<br />", "<br>", "&lt;br/&gt;", "&lt;br /&gt;", "&lt;br&gt;"):
        text = text.replace(tag, "\n")
    return text


def wrap(draw: ImageDraw.ImageDraw, text: str, f: ImageFont.FreeTypeFont,
         width: int, max_lines: int = 2) -> list[str]:
    text = normalize_breaks(text)
    lines: list[str] = []
    current = ""
    consumed = 0
    for consumed, ch in enumerate(text, 1):
        if ch in "\r\n":
            if current:
                lines.append(current)
                current = ""
            elif lines and len(lines) < max_lines:
                lines.append("")
            if len(lines) >= max_lines:
                break
            continue
        candidate = current + ch
        if current and text_width(draw, candidate, f) > width:
            lines.append(current)
            current = ch
            if len(lines) >= max_lines:
                break
        else:
            current = candidate
    if current and len(lines) < max_lines:
        lines.append(current)
    if consumed < len(text) and lines:
        lines[-1] = fit(draw, lines[-1] + "…", f, width)
    return lines


def draw_header(draw: ImageDraw.ImageDraw, title: str) -> None:
    f = ui12(True)
    draw.text((PAD, 8), fit(draw, title, f, W - 2 * PAD - 55), font=f, fill=0)
    draw.line((0, HEADER_H - 2, W, HEADER_H - 2), fill=0, width=1)
    draw.text((W - 48, 13), "87%", font=small(), fill=0)


def draw_footer(draw: ImageDraw.ImageDraw, labels: list[str]) -> None:
    y = H - FOOTER_H
    draw.line((0, y, W, y), fill=0, width=1)
    f = small()
    cell = W / max(1, len(labels))
    for i, label in enumerate(labels):
        box = draw.textbbox((0, 0), label, font=f)
        x = i * cell + (cell - (box[2] - box[0])) / 2
        draw.text((x, y + 10), label, font=f, fill=0)


def render_home() -> Image.Image:
    image = Image.new("L", (W, H), 255)
    draw = ImageDraw.Draw(image)
    draw_header(draw, "晋江文学城")
    y = HEADER_H + 8

    draw.text((PAD, y), "分类热推", font=ui12(True), fill=0)
    y += 34

    categories = ["古代言情", "现代言情", "幻想言情", "纯爱小说",
                  "百合小说", "无CP", "科幻悬疑", "完结精选"]
    columns, rows = 4, 2
    usable = W - 2 * TILE_PAD
    cell_width = (usable - GAP * (columns - 1)) // columns
    cell_height = (TILE_BLOCK_H - GAP * (rows - 1)) // rows

    for i, category in enumerate(categories):
        row, col = divmod(i, columns)
        x = TILE_PAD + col * (cell_width + GAP)
        tile_y = y + row * (cell_height + GAP)
        selected = i == 0
        if selected:
            draw.rounded_rectangle(
                (x + 3, tile_y + 3, x + cell_width - 4, tile_y + cell_height - 4),
                radius=7, fill=232,
            )
        f = ui10(selected)
        label = fit(draw, category, f, cell_width - 8)
        box = draw.textbbox((0, 0), label, font=f)
        tx = x + (cell_width - (box[2] - box[0])) / 2
        ty = tile_y + (cell_height - (box[3] - box[1])) / 2 - 2
        draw.text((tx, ty), label, font=f, fill=0)

    y += TILE_BLOCK_H + 10
    # Gray approximates the firmware's ordered-dithered secondary text.
    draw.text((PAD, y), "古代言情 · 24 本", font=ui10(), fill=85)
    y += 30
    draw.text((PAD, y), "古代言情热推", font=ui10(True), fill=0)
    y += 32

    books = [
        ("01  长风渡：这是一本名字非常长但应该充分使用右侧空间的小说", "墨书白"),
        ("02  穿成反派后我靠种田逆袭成为全京城最受欢迎的人", "某某作者"),
        ("03  长安第一美人", "发达的泪腺"),
        ("04  春日越轨", "慕拉"),
        ("05  我在古代开书局", "青山问我"),
    ]
    title_f = ui10()
    subtitle_f = small()
    row_height = 68
    title_width = W - 2 * PAD - 8

    for index, (title, author) in enumerate(books):
        if y + row_height > H - FOOTER_H:
            break
        if index == 0:
            draw.rounded_rectangle((PAD - 6, y - 3, W - PAD + 3, y + row_height - 4), radius=7, fill=238)
        lines = wrap(draw, title, title_f, title_width, 2)
        if lines:
            draw.text((PAD, y), lines[0], font=title_f, fill=0)
        if len(lines) > 1:
            draw.text((PAD, y + 27), lines[1], font=title_f, fill=0)
        else:
            draw.text((PAD, y + 29), author + " · 连载", font=subtitle_f, fill=90)
        y += row_height

    draw_footer(draw, ["返回", "详情", "刷新"])
    return image


def render_detail() -> Image.Image:
    image = Image.new("L", (W, H), 255)
    draw = ImageDraw.Draw(image)
    draw_header(draw, "晋江文学城")
    y = HEADER_H + 12

    title = "霍格沃茨的学习面板：一段很长的书名也必须允许完整换行"
    for line in wrap(draw, title, ui12(True), W - 2 * PAD, 2):
        draw.text((PAD, y), line, font=ui12(True), fill=0)
        y += 32
    y += 3

    draw.text((PAD, y), "林曦遇鹿", font=ui10(), fill=85)
    y += 27
    draw.text((PAD, y), "官场职场 · 连载 · 131.2万字", font=ui10(), fill=85)
    y += 30
    draw.text((PAD, y), "上次阅读 · 第 137 章  京都夜", font=ui10(True), fill=0)
    y += 36

    # One primary control can keep a light structure; category navigation stays flat.
    draw.rounded_rectangle((PAD, y, W - PAD, y + 48), radius=7, fill=240, outline=0, width=1)
    primary = "继续阅读"
    box = draw.textbbox((0, 0), primary, font=ui12(True))
    draw.text(((W - (box[2] - box[0])) / 2, y + 10), primary, font=ui12(True), fill=0)
    y += 61

    draw.line((PAD, y, W - PAD, y), fill=0)
    y += 12
    draw.text((PAD, y), "最近更新", font=ui10(True), fill=0)
    y += 29
    for item in ["568  任意门", "567  霍格沃茨聊天群", "566  巫师练习中（4k）"]:
        draw.text((PAD, y), item, font=ui10(), fill=0)
        y += 25

    y += 4
    draw.line((PAD, y, W - PAD, y), fill=0)
    y += 12
    draw.text((PAD, y), "简介", font=ui10(True), fill=0)
    y += 29
    intro = (
        "睁开双眼，希恩已成为霍利塞孤儿院的一员，虽然开局有些不妙，"
        "但从未想过的魔法世界正向他打开大门。<br/><br/>"
        "更令人喜悦的是，老家的特产也随之而来。从刚学会荧光咒开始，他一步一步探索这个世界。"
    )
    for line in wrap(draw, intro, ui10(), W - 2 * PAD, 5):
        draw.text((PAD, y), line, font=ui10(), fill=0)
        y += 27

    draw_footer(draw, ["返回", "继续阅读", "章节"])
    return image



def new_canvas():
    image = Image.new("L", (W, H), 255)
    return image, ImageDraw.Draw(image)


def reader_face(bold: bool = False) -> ImageFont.FreeTypeFont:
    return face(READER_BODY_PX, bold)


def draw_progress_bar(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int, fraction: float) -> None:
    draw.rounded_rectangle((x, y, x + w, y + h), radius=h // 2, outline=0, width=1, fill=255)
    if fraction > 0:
        fill_w = max(2, int(w * max(0.0, min(1.0, fraction))) - 2)
        draw.rounded_rectangle((x + 1, y + 1, x + 1 + fill_w, y + h - 1),
                               radius=(h - 1) // 2, fill=160)


def draw_paragraph(draw: ImageDraw.ImageDraw, text: str, x: int, y: int, f: ImageFont.FreeTypeFont,
                   width: int, line_h: int, max_lines: int) -> int:
    for line in wrap(draw, text, f, width, max_lines):
        draw.text((x, y), line, font=f, fill=0)
        y += line_h
    return y

def render_reader() -> Image.Image:
    image, draw = new_canvas()
    draw_header(draw, "长风渡 · 第137章 京都夜")
    body = (
        "长公主府邸的灯火在夜色中次第亮起，顾九思望着案上摊开的舆图，"
        "手指缓缓划过边境线。这一战若胜，北方三镇便可太平十年。<br/><br/>"
        "窗外传来更鼓声，他抬起眼，见玉茹披着斗篷站在廊下，手里提着一盏暖黄的灯。"
        "灯火映着她的眉眼，温柔一如从前。<br/><br/>"
        "“在想什么？”她轻声问。<br/><br/>"
        "他摇了摇头，将舆图轻轻合上，道：“在想，等这一仗打完，便带你去江南看看。”<br/><br/>"
        "春风十里扬州路，卷上珠帘总不如。"
    )
    y = HEADER_H + 14
    y = draw_paragraph(draw, body, PAD, y, reader_face(), W - 2 * PAD, 44, 100)
    prog_y = H - FOOTER_H - 46
    draw_progress_bar(draw, PAD, prog_y, W - 2 * PAD, 6, 0.32)
    draw.text((PAD, prog_y + 14), "第 3 / 288 页 · 32%", font=small(), fill=85)
    draw.text((W - PAD - 150, prog_y + 14), "剩余约 42 分钟", font=small(), fill=85)
    draw_footer(draw, ["目录", "设置", "上一页", "下一页"])
    return image


def render_reader_menu() -> Image.Image:
    image, draw = new_canvas()
    draw.rectangle((0, HEADER_H - 2, W, H - FOOTER_H), fill=226)
    draw.text((PAD, HEADER_H + 28), "…月色如洗，庭院深深，更漏声里漏出几分凉意……",
              font=ui10(), fill=158)
    draw_header(draw, "阅读菜单")
    panel_x = PAD - 8
    panel_w = W - 2 * (PAD - 8)
    top = HEADER_H + 16
    bottom = H - FOOTER_H - 16
    draw.rounded_rectangle((panel_x, top, panel_x + panel_w, bottom), radius=8, fill=255,
                           outline=0, width=1)
    items = [
        ("章节", "目录 · 288 章"),
        ("书签", "3 个"),
        ("字号", "22"),
        ("行距", "标准"),
        ("翻页动画", "关闭"),
        ("自动翻页", "关闭"),
        ("亮度", "关闭"),
        ("阅读进度", "32%"),
    ]
    selected = 2
    row_h = (bottom - top - 20) // len(items)
    for i, (label, value) in enumerate(items):
        ry = top + 10 + i * row_h
        if i == selected:
            draw.rounded_rectangle((panel_x + 6, ry, panel_x + panel_w - 6, ry + row_h - 6),
                                   radius=6, fill=238)
        draw.text((panel_x + 16, ry + (row_h - UI_10_PX) // 2), label,
                  font=ui10(i == selected), fill=0)
        vw = text_width(draw, value, small())
        draw.text((panel_x + panel_w - 16 - vw, ry + (row_h - SMALL_PX) // 2 + 2),
                  value, font=small(), fill=85)
        if i < len(items) - 1:
            draw.line((panel_x + 12, ry + row_h - 4, panel_x + panel_w - 12, ry + row_h - 4),
                      fill=225)
    draw_footer(draw, ["返回", "上一项", "下一项", "确定"])
    return image


def render_settings() -> Image.Image:
    image, draw = new_canvas()
    draw_header(draw, "设置")
    groups = [
        ("阅读与显示", [
            ("全局字体", "系统默认"),
            ("字号", "22"),
            ("行间距", "标准"),
            ("阅读背景", "米白"),
        ]),
        ("网络与应用", [
            ("网络连接", "WiFi · 未连接"),
            ("已安装应用", "6 个"),
        ]),
        ("存储与系统", [
            ("存储空间", "可用 4.1 GB"),
            ("开发者选项", ""),
            ("关于本机", "v1.13.2"),
        ]),
    ]
    selected = "网络连接"
    y = HEADER_H + 12
    for title, rows in groups:
        draw.text((PAD, y), title, font=ui10(True), fill=0)
        y += 27
        for label, value in rows:
            is_sel = label == selected
            if is_sel:
                draw.rounded_rectangle((PAD - 5, y - 2, W - PAD + 5, y + 38), radius=6, fill=238)
            draw.text((PAD, y), label, font=ui10(is_sel), fill=0)
            if value:
                draw.text((W - PAD - text_width(draw, value, small()), y + 2),
                          value, font=small(), fill=85)
            y += 44
        y += 4
    draw_footer(draw, ["返回", "选择", "刷新"])
    return image


def render_library() -> Image.Image:
    image, draw = new_canvas()
    draw_header(draw, "我的书架")
    tabs = ["书架", "最近阅读", "已下载"]
    f = ui10(True)
    cell = (W - 2 * PAD) / len(tabs)
    for i, tab in enumerate(tabs):
        x = PAD + i * cell
        box = draw.textbbox((0, 0), tab, font=f)
        tw = box[2] - box[0]
        if i == 0:
            draw.rounded_rectangle((x + (cell - tw) / 2 - 9, HEADER_H + 8, x + (cell - tw) / 2 + tw + 9, HEADER_H + 38), radius=11, fill=226)
        draw.text((x + (cell - tw) / 2, HEADER_H + 11), tab, font=f, fill=0)
    y = HEADER_H + 48
    books = [
        ("长风渡", "墨书白 · 已读 32%"),
        ("穿成反派后我靠种田逆袭", "某某作者 · 已读 88%"),
        ("长安第一美人", "发达的泪腺 · 未读"),
        ("春日越轨", "慕拉 · 已读 12%"),
        ("霍格沃茨的学习面板", "林曦遇鹿 · 已读 91%"),
        ("我在古代开书局", "青山问我 · 未读"),
    ]
    row_h = 76
    for i, (title, author) in enumerate(books):
        if y + row_h > H - FOOTER_H:
            break
        if i == 0:
            draw.rounded_rectangle((PAD - 6, y - 2, W - PAD + 3, y + row_h - 4), radius=7, fill=238)
        draw.text((PAD, y + 5), title, font=ui10(i == 0), fill=0)
        draw.text((PAD, y + 36), fit(draw, author, small(), W - 2 * PAD - 90),
                  font=small(), fill=90)
        if i == 0:
            draw.text((W - PAD - 60, y + 36), "续读", font=small(True), fill=0)
        y += row_h
    draw_footer(draw, ["返回", "阅读", "管理"])
    return image


def render_apps() -> Image.Image:
    image, draw = new_canvas()
    draw_header(draw, "应用")
    apps = ["番茄小说", "晋江文学城", "微信读书", "本地阅读", "KOReader", "浏览器"]
    cols, rows = 2, 3
    cell_w = (W - 2 * TILE_PAD - GAP) // cols
    cell_h = 156
    y = HEADER_H + 16
    for i, name in enumerate(apps):
        row, col = divmod(i, cols)
        x = TILE_PAD + col * (cell_w + GAP)
        ty = y + row * (cell_h + GAP)
        if i == 0:
            draw.rounded_rectangle((x + 4, ty + 4, x + cell_w - 4, ty + cell_h - 6), radius=10, fill=232)
        gx = x + (cell_w - 40) // 2
        draw.rounded_rectangle((gx, ty + 16, gx + 40, ty + 56), radius=9, fill=205)
        f = ui10(i == 0)
        box = draw.textbbox((0, 0), name, font=f)
        tw = box[2] - box[0]
        draw.text((x + (cell_w - tw) / 2, ty + 64), name, font=f, fill=0)
        draw.text((x + (cell_w - tw) / 2, ty + 92), "已安装", font=small(), fill=130)
    y += rows * cell_h + GAP * (rows - 1) + 18
    draw.line((PAD, y, W - PAD, y), fill=0)
    y += 16
    draw.text((PAD, y), "应用中心", font=ui10(True), fill=0)
    y += 28
    draw.text((PAD, y), "安装新应用 →", font=ui10(), fill=0)
    draw_footer(draw, ["返回", "打开", "安装"])
    return image


def render_network() -> Image.Image:
    image, draw = new_canvas()
    draw_header(draw, "网络设置")
    y = HEADER_H + 14
    draw.text((PAD, y), "Wi-Fi", font=ui10(True), fill=0)
    y += 8
    draw.rounded_rectangle((PAD - 5, y, W - PAD + 5, y + 40), radius=6, fill=238)
    draw.text((PAD, y + 6), "启用 Wi-Fi", font=ui10(True), fill=0)
    draw.rounded_rectangle((W - 76, y + 6, W - 30, y + 34), radius=14, outline=0, width=1, fill=215)
    draw.text((W - 70, y + 11), "开", font=small(True), fill=0)
    y += 52
    draw.line((PAD, y, W - PAD, y), fill=0)
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
    row_h = 56
    for i, (name, state) in enumerate(nets):
        if y + row_h > H - FOOTER_H:
            break
        if i == 0:
            draw.rounded_rectangle((PAD - 5, y - 2, W - PAD + 5, y + row_h - 4), radius=6, fill=238)
        draw.text((PAD, y + 5), name, font=ui10(i == 0), fill=0)
        draw.text((PAD, y + 31), state, font=small(), fill=90)
        y += row_h
    draw_footer(draw, ["返回", "连接", "刷新"])
    return image


# Renderers in canonical set order; --screen all uses this ordering.
SCREENS = [
    ("home", render_home),
    ("detail", render_detail),
    ("reader", render_reader),
    ("reader_menu", render_reader_menu),
    ("settings", render_settings),
    ("library", render_library),
    ("apps", render_apps),
    ("network", render_network),
]


def _write_gallery(out: Path) -> None:
    items = [name for name, _ in SCREENS if (out / f"{name}_fixed_ui.png").exists()]
    if not items:
        return

    def card(name: str) -> str:
        return f'<div class="c"><h3>{name}</h3><img src="{name}_fixed_ui.png" alt="{name}"></div>'

    html = (
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>Murphy M4 UI 预览</title><style>"
        "body{font-family:system-ui,sans-serif;background:#f4f4f4;margin:0;padding:24px}"
        "h1{font-size:20px}.wrap{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:18px}"
        ".c{background:#fff;border:1px solid #ddd;border-radius:10px;padding:12px;box-shadow:0 1px 3px rgba(0,0,0,.1)}"
        "h3{margin:0 0 10px;font-size:15px}img{width:100%;height:auto;border:1px solid #eee;image-rendering:pixelated}"
        "</style></head><body><h1>Murphy M4 UI 预览</h1><div class='wrap'>"
    )
    html += "".join(card(n) for n in items)
    html += "</div></body></html>"
    (out / "index.html").write_text(html, encoding="utf-8")


def render(out: Path, screen: str) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    by_name = dict(SCREENS)
    if screen == "all":
        names = [name for name, _ in SCREENS]
    elif screen == "both":
        names = ["home", "detail"]
    else:
        names = [screen]
    paths: list[Path] = []
    for name in names:
        path = out / f"{name}_fixed_ui.png"
        by_name[name]().save(path)
        paths.append(path)
    _write_gallery(out)
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description="Murphy M4 fixed-raster UI preview workbench")
    choices = [name for name, _ in SCREENS] + ["both", "all"]
    parser.add_argument("--screen", choices=choices, default="all",
                        help="which screen (s) to render (default: all)")
    parser.add_argument("--out", type=Path, default=Path("build/m4ui-preview"))
    parser.add_argument("--watch", action="store_true", help="continuously regenerate PNGs")
    parser.add_argument("--font", type=Path, default=None,
                        help="override CJK regular font file")
    parser.add_argument("--bold", type=Path, default=None,
                        help="override CJK bold font file")
    args = parser.parse_args()

    global REGULAR, BOLD
    if args.font is not None:
        if not args.font.exists():
            raise SystemExit(f"font not found: {args.font}")
        REGULAR = str(args.font)
    if args.bold is not None:
        if not args.bold.exists():
            raise SystemExit(f"bold font not found: {args.bold}")
        BOLD = str(args.bold)

    while True:
        for path in render(args.out, args.screen):
            print(path, flush=True)
        if not args.watch:
            break
        time.sleep(0.5)


if __name__ == "__main__":
    main()
