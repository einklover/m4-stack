#!/usr/bin/env python3
"""Murphy M4 480x800 Native UI preview.

The preview mirrors the fixed runtime-TTF chrome policy used on device:

  SMALL = 18px
  UI_10 = 22px
  UI_12 = 26px

System UI and Native plugins reuse these exact raster sizes; Reader body size is
independent. Category navigation is a flat 4x2 text grid with only the selected
item receiving a subtle gray pill. Secondary metadata is rendered gray here to
approximate the firmware's Bayer-dithered 1-bit treatment.

Examples:
  python tools/m4ui_preview.py --screen both
  python tools/m4ui_preview.py --screen home --watch
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

FONT_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
]
BOLD_CANDIDATES = [
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
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


def render(out: Path, screen: str) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    if screen in ("home", "both"):
        path = out / "home_fixed_ui.png"
        render_home().save(path)
        paths.append(path)
    if screen in ("detail", "both"):
        path = out / "detail_fixed_ui.png"
        render_detail().save(path)
        paths.append(path)
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description="Murphy M4 fixed-raster Native UI preview")
    parser.add_argument("--screen", choices=["home", "detail", "both"], default="both")
    parser.add_argument("--out", type=Path, default=Path("build/m4ui-preview"))
    parser.add_argument("--watch", action="store_true", help="continuously regenerate PNGs")
    args = parser.parse_args()

    while True:
        for path in render(args.out, args.screen):
            print(path, flush=True)
        if not args.watch:
            break
        time.sleep(0.5)


if __name__ == "__main__":
    main()
