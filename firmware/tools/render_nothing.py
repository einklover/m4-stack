#!/usr/bin/env python3
"""Nothing Phone/Watch 风格界面精细渲染 — 像素点阵风"""

from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageOps
from pathlib import Path

W, H = 480, 800
PAD = 30
HEADER_H = 50
FOOTER_H = 46
SMALL_PX = 18
UI_10_PX = 22
UI_12_PX = 26

FONT = "C:/Windows/Fonts/msyh.ttc"
BOLD = "C:/Windows/Fonts/msyhbd.ttc"

def face(px, bold=False):
    return ImageFont.truetype(BOLD if bold else FONT, px)
def ui10(bold=False): return face(UI_10_PX, bold)
def ui12(bold=False): return face(UI_12_PX, bold)
def small(bold=False): return face(SMALL_PX, bold)

def tw(draw, text, f):
    return int(draw.textlength(text, font=f))

def new_canvas(bg=255):
    img = Image.new("L", (W, H), bg)
    return img, ImageDraw.Draw(img)

def draw_header(draw, title, color=0):
    f = ui12(True)
    draw.text((PAD, 8), title, font=f, fill=color)
    draw.line((0, HEADER_H-2, W, HEADER_H-2), fill=color, width=1)
    draw.text((W-48, 13), "87%", font=small(), fill=color)

def draw_footer(draw, labels, color=0):
    y = H - FOOTER_H
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=color)
    n = len(labels)
    if n == 0: return
    cw = W // n
    for i, lbl in enumerate(labels):
        x = cw * i + (cw - tw(draw, lbl, small())) // 2
        draw.text((x, y+12), lbl, font=small(), fill=color)

def draw_dot_matrix(draw, x, y, text, size=10, spacing=12, fill=0):
    """绘制点阵风格文字（每个字符用点阵模拟）"""
    # 使用小型字体模拟点阵效果
    f = face(size, True)
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += tw(draw, ch, f) + 2

def draw_glyph_grid(draw, x, y, cols, rows, cell_w, cell_h, items, fill=0):
    """绘制Glyph风格的网格"""
    for i, (label, sub) in enumerate(items):
        if i >= cols * rows: break
        cx = x + (i % cols) * cell_w
        cy = y + (i // cols) * cell_h
        # 点阵边框
        for bx in range(0, cell_w-4, 4):
            for by in range(0, cell_h-4, 4):
                if bx == 0 or bx >= cell_w-8 or by == 0 or by >= cell_h-8:
                    draw.point((cx+bx, cy+by), fill=fill)
        # 内容
        draw.text((cx+12, cy+12), label, font=ui10(), fill=fill)
        if sub:
            draw.text((cx+12, cy+36), sub, font=small(), fill=180)

# ==================== Nothing 风格渲染 ====================

def render_nothing_home():
    """Nothing Phone — 主屏幕（点阵/极简/黑白）"""
    img, draw = new_canvas(255)
    # 顶部状态栏 — 点阵风格
    f_dot = face(8, True)
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.text((W-80, 12), "●●●○○○", font=f_dot, fill=0)  # 信号
    draw.text((W-45, 12), "▇▇▇▇▇▇", font=f_dot, fill=0)  # 电池
    draw.line((0, 36, W, 36), fill=0, width=2)
    # 大标题
    draw.text((PAD, 56), "好 的 阅 读", font=face(28, True), fill=0)
    draw.text((PAD, 92), "从 这 里 开 始", font=face(14), fill=130)
    # 分隔线（点阵装饰）
    y = 120
    for i in range(W):
        if i % 8 == 0:
            draw.point((i, y), fill=0)
    # 点阵时钟Glyph
    y = 140
    glyphs = [
        ("📖  正 在 阅 读", "三体 · 刘慈欣  ───  75%"),
        ("📚  我 的 书 架", "42 本 书"),
        ("⏱  最 近 阅 读", "8 本 未 读"),
        ("⚙  快 速 操 作", ""),
    ]
    for i, (label, sub) in enumerate(glyphs):
        if y + 64 > H - 60: break
        # 点阵边框
        draw.rectangle((PAD-5, y, W-PAD+5, y+56), fill=245)
        for bx in range(PAD-5, W-PAD+5, 4):
            draw.point((bx, y), fill=0)
            draw.point((bx, y+56), fill=0)
        for by in range(y, y+56, 4):
            draw.point((PAD-5, by), fill=0)
            draw.point((W-PAD+5, by), fill=0)
        draw.text((PAD, y+10), label, font=ui10(True), fill=0)
        if sub:
            draw.text((PAD, y+34), sub, font=small(), fill=130)
        y += 66
    # 底部导航（点阵风格）
    y = H - 56
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=0, width=2)
    navs = ["[  首 页  ]", "[  书 库  ]", "[  设 置  ]", "[  关 于  ]"]
    cw = W // 4
    for i, nav in enumerate(navs):
        draw.text((cw*i+8, y+14), nav, font=face(8, True), fill=0)
    return img

def render_nothing_library():
    """Nothing Phone — 书架（点阵网格）"""
    img, draw = new_canvas(255)
    # 顶部
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.line((0, 36, W, 36), fill=0, width=2)
    draw.text((PAD, 50), "我 的 书 架", font=face(22, True), fill=0)
    draw.text((W-PAD-60, 54), "42 本", font=small(), fill=130)
    # 搜索
    draw.rectangle((PAD-5, 76, W-PAD+5, 108), fill=243)
    draw.text((PAD, 84), "🔍 搜 索 书 籍", font=small(), fill=150)
    draw.line((PAD-5, 108, W-PAD+5, 108), fill=0, width=1)
    # 书架网格
    y = 120
    books = [("三体", "刘慈欣", "75%"), ("活着", "余华", "32%"),
             ("百年孤独", "马尔克斯", "88%"), ("人类简史", "赫拉利", "45%"),
             ("时间简史", "霍金", "60%"), ("围城", "钱钟书", "12%"),
             ("道德经", "老子", "28%"), ("红楼梦", "曹雪芹", "55%")]
    cw = (W - 2*PAD - 8) // 2
    for i, (title, author, pct) in enumerate(books):
        if i >= 8: break
        cx = PAD + (i % 2) * (cw + 8)
        cy = y + (i // 2) * 72
        if cy + 68 > H - 56: break
        # 点阵方块
        draw.rectangle((cx, cy, cx+cw, cy+68), fill=245)
        for bx in range(cx, cx+cw, 4):
            draw.point((bx, cy), fill=0)
            draw.point((bx, cy+68), fill=0)
        for by in range(cy, cy+68, 4):
            draw.point((cx, by), fill=0)
            draw.point((cx+cw, by), fill=0)
        # 封面缩略图（点阵纹理）
        draw.rectangle((cx+6, cy+6, cx+28, cy+62), fill=230)
        for tx in range(cx+6, cx+28, 3):
            for ty in range(cy+6, cy+62, 3):
                if (tx+ty) % 6 == 0:
                    draw.point((tx, ty), fill=200)
        # 文字
        draw.text((cx+34, cy+8), title, font=ui10(True), fill=0)
        draw.text((cx+34, cy+32), author, font=small(), fill=130)
        draw.text((cx+34, cy+50), pct, font=small(), fill=0)
    # 底部
    y = H - 56
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=0, width=2)
    navs = ["[  首 页  ]", "[  书 库  ]", "[  设 置  ]", "[  关 于  ]"]
    cw = W // 4
    for i, nav in enumerate(navs):
        draw.text((cw*i+8, y+14), nav, font=face(8, True), fill=0)
    return img

def render_nothing_reader():
    """Nothing Phone — 阅读页"""
    img, draw = new_canvas(255)
    # 顶部
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.line((0, 36, W, 36), fill=0, width=2)
    draw.text((PAD, 46), "三 体", font=face(14, True), fill=0)
    draw.text((W-PAD-60, 48), "75%", font=small(), fill=130)
    draw.line((0, 70, W, 70), fill=240, width=1)
    # 正文（打字机/点阵风格）
    y = 84
    text_lines = [
        "第 一 章  科 学 边 界",
        "",
        "  宇 宙 就 是 一 座 黑 暗 森 林 ,",
        "  每 个 文 明 都 是 带 枪 的 猎 人 ,",
        "  像 幽 灵 般 潜 行 于 林 间 ,",
        "  轻 轻 拨 开 挡 路 的 树 枝 ,",
        "  竭 力 不 让 脚 步 发 出 一 点 声 音 ,",
        "  连 呼 吸 都 必 须 小 心 翼 翼 。",
        "",
        "  他 必 须 小 心 , 因 为 林 中 到 处",
        "  都 有 与 他 一 样 潜 行 的 猎 人 。",
    ]
    f = face(12)
    for line in text_lines:
        draw.text((PAD, y), line, font=f, fill=0)
        y += 24
    # 底部进度条（点阵）
    y = H - 50
    draw.line((0, y, W, y), fill=240, width=1)
    # 点阵进度条
    bar_w = W - 2*PAD
    for i in range(0, bar_w, 4):
        if i < int(bar_w * 0.75):
            draw.rectangle((PAD+i, y+8, PAD+i+2, y+16), fill=0)
        else:
            draw.rectangle((PAD+i, y+8, PAD+i+2, y+16), fill=220)
    draw.text((PAD, y+22), "[  ◀  翻 页  ]", font=face(8, True), fill=0)
    draw.text((W-PAD-100, y+22), "[  翻 页  ▶  ]", font=face(8, True), fill=0)
    return img

def render_nothing_reader_menu():
    """Nothing Phone — 阅读菜单"""
    img, draw = new_canvas(255)
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.line((0, 36, W, 36), fill=0, width=2)
    draw.text((PAD, 50), "阅 读 菜 单", font=face(22, True), fill=0)
    y = 90
    items = [
        ("📖  目    录", "跳 转 章 节"),
        ("🔖  书    签", "管 理 标 记"),
        ("⚙  阅 读 设 置", "字 体 / 行 距"),
        ("📤  分    享", "推 荐 好 书"),
        ("🔍  搜    索", "查 找 内 容"),
        ("📊  阅 读 统 计", "今 日 阅 读 15 分 钟"),
    ]
    for i, (label, sub) in enumerate(items):
        if y + 48 > H - 56: break
        draw.rectangle((PAD-5, y, W-PAD+5, y+44), fill=245)
        # 点阵装饰
        for bx in range(PAD-5, W-PAD+5, 8):
            draw.point((bx, y), fill=0)
            draw.point((bx, y+44), fill=0)
        draw.text((PAD, y+8), label, font=ui10(True), fill=0)
        draw.text((W-PAD-80, y+8), sub, font=small(), fill=130)
        y += 48
    # 底部
    y = H - 56
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=0, width=2)
    draw.text((PAD+10, y+14), "[  返 回  ]", font=face(8, True), fill=0)
    draw.text((W//2-30, y+14), "[  确 认  ]", font=face(8, True), fill=0)
    draw.text((W-PAD-100, y+14), "[  向 下  ]", font=face(8, True), fill=0)
    return img

def render_nothing_settings():
    """Nothing Phone — 设置（点阵风格）"""
    img, draw = new_canvas(255)
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.line((0, 36, W, 36), fill=0, width=2)
    draw.text((PAD, 50), "设    置", font=face(22, True), fill=0)
    # 标签栏
    tabs = [("显示", True), ("按钮", False), ("系统", False)]
    x = PAD
    for label, sel in tabs:
        if sel:
            draw.rectangle((x, 80, x+80, 108), fill=0)
            draw.text((x+12, 86), label, font=ui10(True), fill=255)
        else:
            draw.rectangle((x, 80, x+80, 108), fill=240)
            draw.text((x+12, 86), label, font=ui10(True), fill=0)
        x += 88
    y = 120
    items = [
        ("锁 屏 壁 纸", "透 明 叠 加"),
        ("阅 读 进 度", "无 进 度"),
        ("刷 新 频 率", "5 页 全 刷"),
        ("电 池 百 分 比", "隐 藏"),
        ("前 光 亮 度", "60"),
        ("前 光 色 温", "50"),
        ("图 片 质 量", "普 通"),
        ("图 标 风 格", "点 阵"),
    ]
    for i, (name, val) in enumerate(items):
        if y + 36 > H - 56: break
        if i == 0:
            draw.rectangle((PAD-5, y, W-PAD+5, y+32), fill=238)
        draw.text((PAD, y+6), name, font=ui10(i==0), fill=0)
        draw.text((W-PAD-80, y+6), val, font=small(), fill=130)
        # 点阵分隔线
        if i < len(items) - 1:
            for dx in range(PAD-5, W-PAD+5, 6):
                draw.point((dx, y+34), fill=230)
        y += 36
    # 底部
    y = H - 56
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=0, width=2)
    draw.text((PAD+10, y+14), "[  返 回  ]", font=face(8, True), fill=0)
    draw.text((W//2-30, y+14), "[  修 改  ]", font=face(8, True), fill=0)
    draw.text((W-PAD-100, y+14), "[  向 下  ]", font=face(8, True), fill=0)
    return img

def render_nothing_watch():
    """Nothing Watch — 手表风格界面（圆形点阵）"""
    img, draw = new_canvas(255)
    # 模拟圆形表盘（在480x800上做方形布局）
    # 顶部 — 时间大显示
    draw.text((PAD, 20), "10 : 30", font=face(36, True), fill=0)
    draw.text((PAD, 65), "星 期 四  08-27", font=face(10), fill=130)
    # 点阵分隔
    for dx in range(0, W, 6):
        draw.point((dx, 90), fill=0)
    # 功能卡片（点阵边框）
    cards = [
        ("📖  阅 读", "今 日 15 分 钟"),
        ("📚  书 架", "42 本"),
        ("⏱  进 度", "三 体  75%"),
        ("⚙  快 捷", "随 时 开 始"),
    ]
    y = 110
    cw = (W - 2*PAD - 8) // 2
    for i, (label, sub) in enumerate(cards):
        cx = PAD + (i % 2) * (cw + 8)
        cy = y + (i // 2) * 100
        # 点阵圆角矩形
        draw.rectangle((cx, cy, cx+cw, cy+92), fill=245)
        for bx in range(cx, cx+cw, 4):
            draw.point((bx, cy), fill=0)
            draw.point((bx, cy+92), fill=0)
        for by in range(cy, cy+92, 4):
            draw.point((cx, by), fill=0)
            draw.point((cx+cw, by), fill=0)
        draw.text((cx+12, cy+20), label, font=ui10(True), fill=0)
        draw.text((cx+12, cy+52), sub, font=small(), fill=130)
    # 底部状态
    y = y + 210
    draw.line((0, y, W, y), fill=240, width=1)
    y += 10
    # 步数/心率等（点阵）
    stats = [
        ("❤  心 率", "72 bpm"),
        ("🚶  步 数", "6,382"),
    ]
    for i, (label, val) in enumerate(stats):
        draw.text((PAD + i*200, y), label, font=small(), fill=130)
        draw.text((PAD + i*200, y+24), val, font=ui10(True), fill=0)
    return img

def render_nothing_boot():
    """Nothing Phone — 开机动画风格"""
    img, draw = new_canvas(255)
    # 纯黑背景
    draw.rectangle((0, 0, W, H), fill=0)
    # 点阵logo
    logo = [
        "  ███  █   █ █████ █   █ █████ ",
        "  █   █ █   █ █     ██  █ █     ",
        "  █   █ █   █ ███   █ █ █ ███   ",
        "  █   █ █   █ █     █  ██ █     ",
        "  ███   ███  █████ █   █ █████ ",
        "",
        "  ███   ███  ████  █   █ █████ ",
        "  █   █ █   █ █   █ █   █ █     ",
        "  █     █   █ ████  █████ ███   ",
        "  █   █ █   █ █   █ █   █ █     ",
        "  ███   ███  █   █ █   █ █████ ",
    ]
    f_dot = face(8)
    y = 240
    for line in logo:
        draw.text((60, y), line, font=f_dot, fill=255)
        y += 14
    # 底部加载
    draw.text((W//2-60, 500), "LOADING...", font=face(10, True), fill=255)
    # 点阵进度条
    for i in range(0, 240, 4):
        if i < 160:
            draw.rectangle((120+i, 530, 120+i+2, 540), fill=255)
        else:
            draw.rectangle((120+i, 530, 120+i+2, 540), fill=40)
    return img

def render_nothing_glyph():
    """Nothing Phone — Glyph 灯光界面风格"""
    img, draw = new_canvas(255)
    # 顶部
    draw.text((PAD, 8), "10:30", font=face(18, True), fill=0)
    draw.line((0, 36, W, 36), fill=0, width=2)
    # Glyph 灯光图案
    y = 60
    draw.text((PAD, y), "GLYPH 界 面", font=face(20, True), fill=0)
    y += 50
    # 发光条模拟
    glyph_bars = [
        ("━━━━━━━━━━━━━━━━━━━", "通 知 提 醒"),
        ("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌", "阅 读 模 式"),
        ("━━━━━━━━━━━━━━━━━━━", "充 电 状 态"),
        ("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌", "音 量 调 节"),
    ]
    f_glyph = face(14, True)
    for bar, label in glyph_bars:
        if y + 60 > H - 56: break
        draw.text((PAD, y), bar, font=f_glyph, fill=0)
        draw.text((PAD, y+24), label, font=small(), fill=130)
        y += 56
    # 底部
    y = H - 56
    draw.rectangle((0, y, W, H), fill=240)
    draw.line((0, y, W, y), fill=0, width=2)
    navs = ["[  GLYPH  ]", "[  灯 光  ]", "[  设 置  ]", "[  关 于  ]"]
    cw = W // 4
    for i, nav in enumerate(navs):
        draw.text((cw*i+8, y+14), nav, font=face(8, True), fill=0)
    return img


# ==================== 输出 ====================

OUT = Path(__file__).parent.parent / "build" / "m4ui-preview" / "reference"
SCREENS = [
    ("nothing_home", render_nothing_home, "Nothing Phone 主屏幕"),
    ("nothing_library", render_nothing_library, "Nothing 书架"),
    ("nothing_reader", render_nothing_reader, "Nothing 阅读页"),
    ("nothing_reader_menu", render_nothing_reader_menu, "Nothing 阅读菜单"),
    ("nothing_settings", render_nothing_settings, "Nothing 设置"),
    ("nothing_watch", render_nothing_watch, "Nothing Watch 手表"),
    ("nothing_boot", render_nothing_boot, "Nothing 开机动画"),
    ("nothing_glyph", render_nothing_glyph, "Nothing Glyph 灯光"),
]

def write_gallery(out):
    items = [(n, l) for n, _, l in SCREENS if (out / f"{n}.png").exists()]
    def card(n, l):
        return f'<div class="c"><h3>{l}</h3><img src="{n}.png" alt="{l}"></div>'
    html = (
        '<!doctype html><html><head><meta charset="utf-8">'
        '<title>Nothing 风格 UI 参考</title><style>'
        'body{font-family:system-ui,sans-serif;background:#f4f4f4;margin:0;padding:24px}'
        'h1{font-size:22px}p.d{color:#666;font-size:13px;margin:0 0 18px}'
        '.wrap{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:16px}'
        '.c{background:#fff;border:1px solid #ddd;border-radius:10px;padding:12px;box-shadow:0 2px 6px rgba(0,0,0,.1)}'
        'h3{margin:0 0 8px;font-size:15px}'
        'img{width:100%;height:auto;border:1px solid #eee;border-radius:4px}'
        '</style></head><body>'
        '<h1>Nothing Phone / Watch 风格 UI 参考</h1>'
        f'<p class="d">共 {len(items)} 屏 · 像素点阵风 · 黑白极简 · Glyph 灯光元素</p><div class="wrap">'
    )
    html += "".join(card(n, l) for n, l in items)
    html += "</div></body></html>"
    (out / "nothing-index.html").write_text(html, encoding="utf-8")

def main():
    out = OUT
    out.mkdir(parents=True, exist_ok=True)
    for name, fn, label in SCREENS:
        path = out / f"{name}.png"
        img = fn()
        img.save(path)
        print(f"  ✓ {label}  ({path})")
    write_gallery(out)
    print(f"\n  → 画廊: {out / 'nothing-index.html'}")

if __name__ == "__main__":
    main()
