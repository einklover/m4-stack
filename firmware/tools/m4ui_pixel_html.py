#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Murphy M4 像素点阵风格 HTML 预览生成器 — 58 页面适配。"""
from __future__ import annotations
from pathlib import Path
import re

OUT = Path(__file__).resolve().parent.parent / "build" / "m4ui-pixel"

CSS = """*{margin:0;padding:0;box-sizing:border-box}
body{background:#fff;font-family:'Courier New',Courier,monospace;display:flex;justify-content:center;padding:20px 0}
.page{width:400px;min-height:800px;overflow:hidden;background:#fff}
.dotgrid{background:#fff}
.px-icon{display:inline-grid;grid-template-columns:repeat(8,4px);grid-template-rows:repeat(8,4px);gap:1px}
.px-icon>i{display:block;width:4px;height:4px;background:#111}
.px-icon>i.v{background:transparent}
.pixel-cover{display:inline-grid;grid-template-columns:repeat(12,4px);grid-template-rows:repeat(16,4px);gap:1px}
.pixel-cover>i{display:block;width:4px;height:4px;background:transparent}
.pixel-cover>i.f{background:#111}
.pixel-cover>i.m{background:#D0D0D0}
.track-x{letter-spacing:0.1em}
.track-m{letter-spacing:0.08em}
.border-line{border:1px solid #C0C0C0}
.bg-card{background:#fff}
.px-sep{height:1px;background:repeating-linear-gradient(90deg,#111 0,#111 3px,transparent 3px,transparent 7px)}
.dot-prog{display:flex;align-items:center;gap:2px}
.dot-prog>i{display:block;width:6px;height:6px;border-radius:50%;background:#111}
.dot-prog>i.o{background:#D0D0D0}
.glyph-line{display:flex;align-items:center;gap:4px}
.glyph-line>span{display:block;height:2px}
.nav-item{font-size:11px;letter-spacing:0.15em;color:#111;cursor:pointer;padding:4px 10px;border:1px solid #C0C0C0;border-radius:4px;background:#fff}
.nav-item.act{background:#111;color:#fff;font-weight:bold;border-color:#111}
.seg-tabs .nav-item{font-size:11px;letter-spacing:0.15em;color:#111;cursor:pointer;padding:4px 10px;border:1px solid #C0C0C0;border-radius:4px;background:#fff}
.seg-tabs .nav-item.active{background:#111;color:#fff;font-weight:bold;border-color:#111}
/* 列表/目录项：参照主页卡片 —— 白底 + #C0C0C0 描边 + 间距分开；选中=墨色描边+加粗 */
.list-item{font-size:11px;letter-spacing:0.08em;padding:8px 12px;cursor:pointer;border:1px solid #C0C0C0;background:#fff;margin-top:8px}
.list-item.sel{border-color:#111;font-weight:bold;color:#111}
.list-item .sub{font-size:10px;color:#111;letter-spacing:0.05em;margin-top:2px}
.list-item .r{color:#111;font-size:10px;text-align:right}
.btn-dark{background:#111;color:#fff;border-radius:6px;padding:10px 24px;font-size:12px;font-weight:bold;letter-spacing:0.1em;cursor:pointer;text-align:center;border:none}
.btn-light{background:#fff;color:#111;border-radius:6px;padding:10px 24px;font-size:12px;font-weight:bold;letter-spacing:0.1em;cursor:pointer;text-align:center;border:1px solid #C0C0C0}
.prog-bar{height:6px;border-radius:3px;background:#D0D0D0}
.prog-bar>div{height:6px;border-radius:3px;background:#111}
.tag{font-size:9px;border:1px solid #111;padding:1px 8px;letter-spacing:0.2em;color:#111}
.f10{font-size:10px}
.f11{font-size:11px}
.f13{font-size:13px}
.f16{font-size:16px}
.c555{color:#111}
.c888{color:#111}
.c111{color:#111}
.b{font-weight:bold}

.flex{display:flex}.flex-1{flex:1}.flex-col{flex-direction:column}
.items-center{align-items:center}.justify-between{justify-content:space-between}
.justify-center{justify-content:center}.text-center{text-align:center}
.gap-1{gap:4px}.gap-1\.5{gap:6px}.gap-2{gap:8px}.gap-2\.5{gap:10px}.gap-3{gap:12px}.gap-4{gap:16px}.gap-6{gap:24px}
.inline-flex{display:inline-flex}
.w-2{width:8px}.w-2\\.5{width:10px}.w-6{width:24px}.w-10{width:40px}.w-48{width:192px}.w-full{width:100%}
.h-2{height:8px}.h-2\\.5{height:10px}.h-\[1px\]{height:1px}
.mx-4{margin-left:16px;margin-right:16px}.mx-auto{margin-left:auto;margin-right:auto}
.mt-auto{margin-top:auto}.my-3{margin-top:12px;margin-bottom:12px}
.mt-0\.5{margin-top:2px}.mt-1{margin-top:4px}.mt-1\.5{margin-top:6px}.mt-2{margin-top:8px}.mt-3{margin-top:12px}.mt-4{margin-top:16px}.mt-6{margin-top:24px}.mt-8{margin-top:32px}
.mb-2{margin-bottom:8px}.mb-3{margin-bottom:12px}.mb-4{margin-bottom:16px}.mb-6{margin-bottom:24px}
.ml-1{margin-left:4px}.ml-2{margin-left:8px}.ml-4{margin-left:16px}.ml-6{margin-left:24px}
.pt-1{padding-top:4px}.pt-3{padding-top:12px}.pt-4{padding-top:16px}.pt-8{padding-top:32px}.pt-10{padding-top:40px}
.pb-1{padding-bottom:4px}.pb-2{padding-bottom:8px}.pb-3{padding-bottom:12px}.pb-4{padding-bottom:16px}
.px-1{padding-left:4px;padding-right:4px}.px-1\.5{padding-left:6px;padding-right:6px}
.px-2{padding-left:8px;padding-right:8px}.px-3{padding-left:12px;padding-right:12px}
.px-4{padding-left:16px;padding-right:16px}.px-6{padding-left:24px;padding-right:24px}
.py-0\.5{padding-top:2px;padding-bottom:2px}.py-1{padding-top:4px;padding-bottom:4px}.py-2{padding-top:8px;padding-bottom:8px}.py-2\.5{padding-top:10px;padding-bottom:10px}
.py-3{padding-top:12px;padding-bottom:12px}.py-8{padding-top:32px;padding-bottom:32px}
.p-0{padding:0}.p-2\.5{padding:10px}.p-3{padding:12px}.p-4{padding:16px}.rounded-xl{border-radius:12px}
.rounded-full{border-radius:9999px}.border{border:1px solid #C0C0C0}
.leading-relaxed{line-height:1.7}.space-y-3>*+*{margin-top:12px}
.text-white{color:#fff}.font-bold{font-weight:bold}
.text-\\[7px\\]{font-size:7px}.text-\\[8px\\]{font-size:8px}
.text-\\[9px\\]{font-size:9px}.text-\[10px\]{font-size:10px}
.text-\[11px\]{font-size:11px}.text-\[12px\]{font-size:12px}
.text-\[13px\]{font-size:13px}.text-\[14px\]{font-size:14px}
.text-\[16px\]{font-size:16px}.text-\[18px\]{font-size:18px}.text-\[22px\]{font-size:22px}
.tracking-\[0\.1em\]{letter-spacing:0.1em}
.tracking-\[0\.12em\]{letter-spacing:0.12em}
.tracking-\[0\.15em\]{letter-spacing:0.15em}
.tracking-\[0\.2em\]{letter-spacing:0.2em}
.tracking-\[0\.3em\]{letter-spacing:0.3em}
.bg-\[#111\]{background:#111}
.bg-\[#C0C0C0\]{background:#C0C0C0}
.bg-white{background:#fff}.bg-black{background:#000}
.border-\[#111\]{border-color:#111}
.border-\[#C0C0C0\]{border-color:#C0C0C0}
.text-\[#111\]{color:#111}.text-\[#555\]{color:#555}
.text-\[#888\]{color:#888}
.opacity-40{opacity:0.4}
"""

# Pixel logo: 8x8 M shape
PX_M = "".join(["<i></i>"]*8+["<i></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i></i>","<i></i>"]*4+["<i></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i class=v></i>","<i></i>","<i></i>"]+["<i></i>"]*8)

def make_cover(fn):
    return "".join('<i class="f"></i>' if (x==0 or x==11 or y==0 or y==15 or fn(x,y)) else '<i class="m"></i>' for y in range(16) for x in range(12))

COVERS = {
    "santi": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (4<=x<=7 and 4<=y<=7) or (x==5 and 9<=y<=11) or (3<=x<=5 and y==10))),
    "huozhe": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (4<=x<=7 and y in(4,7,10,12)) or (x==4 and 5<=y<=6) or (x==7 and 5<=y<=6))),
    "bainian": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x-5.5)**2+(y-7.5)**2 <= 12)),
    "juren": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or abs(x-y)<=1)),
    "daode": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x in(4,7) and 4<=y<=7) or (x==5 and y in(4,7,10)))),
    "shijian": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x-5.5)**2+(y-4.5)**2<=6)),
    "weicheng": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x in(3,8) and 4<=y<=7) or (x in(4,7) and 5<=y<=6))),
    "honglou": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x in(4,5,6,7) and y in(4,5,6,7,10,11,12,13)))),
    "renlei": make_cover(lambda x,y: 2<=x<=9 and 2<=y<=13 and (x==2 or x==9 or y==2 or y==13 or (x==3 and 4<=y<=7) or (x==8 and 4<=y<=7) or (x in(4,5,6,7) and y==4) or (x in(4,5,6,7) and y==7))),
}

def cover_for(key, cols, rows):
    """按完整 <i> 标签安全截取指定尺寸的像素封面。"""
    return "".join(re.findall(r'<i[^>]*></i>', COVERS[key])[:cols * rows])

def dots(n, total):
    return "".join("<i></i>" if i < n else '<i class="o"></i>' for i in range(total))

def dots_from_pct(pct, total=9):
    return dots(max(1, round(pct/100*total)), total)

# Component functions
def header(title="", subtitle="", battery="87%"):
    g = '<div class="mt-3" style="height:2px;background:#111"></div>'
    return f'''<div class="dotgrid px-4 pt-4 pb-3">
<header><div class="flex items-center justify-between">
<div class="flex items-center gap-2.5">
<div class="px-icon">{PX_M}</div>
<div><div class="text-[13px] font-bold" style="letter-spacing:0.12em">墨菲M4</div><div class="text-[8px] text-[#111] mt-0.5 tracking-[0.15em]">CrossPoint</div></div>
</div>
<div class="flex gap-1.5"><span class="w-2.5 h-2.5 bg-[#111]"></span><span class="w-2.5 h-2.5 border border-[#111]"></span></div>
</div>
{g}</header></div>'''

def header_bar(title):
    # title 保留参数以兼容既有调用，但不再渲染页面说明文字
    g = '<div class="mt-3" style="height:2px;background:#111"></div>'
    return f'''<div class="dotgrid px-4 pt-4 pb-3"><header><div class="flex items-center justify-between">
<div><div class="text-[13px] font-bold" style="letter-spacing:0.12em">墨菲M4</div><div class="text-[8px] text-[#111] mt-0.5 tracking-[0.15em]">CrossPoint</div></div>
<div class="flex gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
</div>{g}</header></div>'''

def footer(labels):
    visible = [l for l in labels if l]
    items = "".join(f'<span class="nav-item {"act" if i==0 else ""}">{l}</span>' for i,l in enumerate(visible))
    return f'<div class="flex justify-center gap-2 mt-auto pt-3">{items}</div>' if items else ""

def sec_title(title, badge=""):
    b = f'<span class="tag ml-2">{badge}</span>' if badge else ""
    return f'<div class="flex items-center gap-3 pt-3 px-4"><h2 class="track-x text-[16px] font-bold">{title}</h2>{b}</div>'

def book_card(cover, title, author, pct, time):
    ds = dots_from_pct(pct)
    return f'''<div class="border-line bg-card mx-4 mt-3 p-0 flex">
<div class="p-2.5 flex items-center"><div class="pixel-cover">{cover}</div></div>
<div class="flex-1 py-3 pl-2 pr-3 flex flex-col justify-between">
<div><div class="flex items-center gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
<div class="mt-2"><span class="text-[16px] font-bold track-m">{title}</span></div>
<div class="text-[11px] text-[#555] mt-0.5 tracking-[0.1em]">{author}</div></div>
<div class="flex items-center justify-between mt-2">
<div class="dot-prog flex items-center gap-1">{ds}<span class="text-[10px] text-[#555] ml-1 tracking-[0.1em]">{pct}%</span></div>
<span class="text-[13px] font-bold tracking-[0.1em] text-[#555]">{time}</span>
</div></div></div>'''

def list_item(name, sub="", sel=False, right=""):
    sel_cls = "sel" if sel else ""
    rt = f'<span class="r">{right}</span>' if right else ""
    sb = f'<div class="sub">{sub}</div>' if sub else ""
    return f'<div class="flex items-center list-item {sel_cls}"><div class="flex-1"><span class="f11 b">{"" if sel else ""}{name}</span>{sb}</div>{rt}</div>'

def settings_list(items, sel=0):
    return "\n".join(f'<div class="flex items-center justify-between list-item {"sel" if i==sel else ""}"><span class="f11 b">{"" if i==sel else ""}{n}</span><span class="f10 c555">{v}</span></div>' for i,(n,v) in enumerate(items))

def divider():
    return '<div class="px-sep mx-4 my-3"></div>'

def section(title, content):
    h = f'<div class="track-x text-[11px] font-bold pt-3 pb-1 px-4">{title}</div>' if title else ""
    return f'{h}{content}'

def tab_bar(tabs, active):
    items = "".join(f"<span class=\"nav-item {('active' if a==active else '')}\">{l}</span>" for a,l in enumerate(tabs))
    return f'<div class="seg-tabs flex justify-center gap-2 mt-3">{items}</div>'

def progress_bar(pct):
    return f'<div class="prog-bar w-full"><div style="width:{pct}%"></div></div>'

def stat_card(label, value, sub=""):
    sb = f'<div class="text-[7px] tracking-[0.15em] text-[#555] mt-1">{sub}</div>' if sub else ""
    return f'''<div class="flex-1 text-center px-1.5 py-2.5 bg-white border border-[#C0C0C0]">
<div class="text-[8px] tracking-[0.15em] text-[#555]">{label}</div>
<div class="text-[13px] font-bold tracking-[0.1em] text-[#111] mt-1">{value}</div>
<div class="w-6 h-[1px] bg-[#C0C0C0] mx-auto mt-1.5"></div>{sb}
</div>'''

def page_wrap(content):
    return f'<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=400"><title>Murphy M4 Pixel UI</title><style>{CSS}</style></head><body><div class="page">{content}</div></body></html>'

PAGES = []
def reg(name, label, fn):
    PAGES.append((name, label, fn))

# === PAGES 1-20 ===

# 1. Home
reg("home", "主页", lambda: page_wrap(header() + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px)">
<div class="flex items-center justify-between px-4 pt-3 pb-1">
<h2 class="track-x text-[16px] font-bold">我 的 书 架</h2>
<span class="tag">4 本</span>
</div>
<div class="border-line bg-card mx-4 mt-3 flex">
<div class="p-2 flex items-center"><div class="pixel-cover">{COVERS["santi"]}</div></div>
<div class="flex-1 py-3 pl-2 pr-3 flex flex-col justify-between">
<div class="flex items-center gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
<div class="mt-2"><span class="text-[16px] font-bold track-m">三 体</span></div>
<div class="text-[11px] text-[#555] mt-1 tracking-[0.1em]">刘慈欣</div>
<div class="flex items-center justify-between mt-2">
<div class="dot-prog flex items-center gap-1">{dots_from_pct(75)}<span class="text-[10px] text-[#555] ml-1 tracking-[0.1em]">75%</span></div>
<span class="text-[13px] font-bold tracking-[0.1em] text-[#555]">25 min</span>
</div></div></div>
<div class="border-line bg-card mx-4 mt-3 flex">
<div class="p-2 flex items-center"><div class="pixel-cover">{COVERS["huozhe"]}</div></div>
<div class="flex-1 py-3 pl-2 pr-3 flex flex-col justify-between">
<div class="flex items-center gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
<div class="mt-2"><span class="text-[16px] font-bold track-m">活 着</span></div>
<div class="text-[11px] text-[#555] mt-1 tracking-[0.1em]">余华</div>
<div class="flex items-center justify-between mt-2">
<div class="dot-prog flex items-center gap-1">{dots_from_pct(32)}<span class="text-[10px] text-[#555] ml-1 tracking-[0.1em]">32%</span></div>
<span class="text-[13px] font-bold tracking-[0.1em] text-[#555]">12 min</span>
</div></div></div>
<div class="border-line bg-card mx-4 mt-3 flex">
<div class="p-2 flex items-center"><div class="pixel-cover">{COVERS["bainian"]}</div></div>
<div class="flex-1 py-3 pl-2 pr-3 flex flex-col justify-between">
<div class="flex items-center gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
<div class="mt-2"><span class="text-[16px] font-bold track-m">百 年 孤 独</span></div>
<div class="text-[11px] text-[#555] mt-1 tracking-[0.1em]">马尔克斯</div>
<div class="flex items-center justify-between mt-2">
<div class="dot-prog flex items-center gap-1">{dots_from_pct(88)}<span class="text-[10px] text-[#555] ml-1 tracking-[0.1em]">88%</span></div>
<span class="text-[13px] font-bold tracking-[0.1em] text-[#555]">38 min</span>
</div></div></div>
<div class="border-line bg-card mx-4 mt-3 flex">
<div class="p-2 flex items-center"><div class="pixel-cover">{COVERS["juren"]}</div></div>
<div class="flex-1 py-3 pl-2 pr-3 flex flex-col justify-between">
<div class="flex items-center gap-1"><span class="w-2 h-2 bg-[#111]"></span><span class="w-2 h-2 border border-[#111]"></span></div>
<div class="mt-2"><span class="text-[16px] font-bold track-m">局 外 人</span></div>
<div class="text-[11px] text-[#555] mt-1 tracking-[0.1em]">加缪</div>
<div class="flex items-center justify-between mt-2">
<div class="dot-prog flex items-center gap-1">{dots_from_pct(15)}<span class="text-[10px] text-[#555] ml-1 tracking-[0.1em]">15%</span></div>
<span class="text-[13px] font-bold tracking-[0.1em] text-[#555]">8 min</span>
</div></div></div>
{divider()}
<div class="flex gap-2 px-4 mt-3">
{stat_card("累计阅读时长", "12 h 38 min", "本周")}
{stat_card("上次阅读时长", "25 min", "三体·今晚<br>20:15")}
{stat_card("日均阅读", "42 min", "本周趋势 ↑")}
</div>
{divider()}
<div class="flex justify-center gap-2 mt-3">
<span class="nav-item">首页</span><span class="nav-item act">书架</span><span class="nav-item">最近</span><span class="nav-item">设置</span>
</div>
<div class="glyph-line justify-center opacity-40 mt-2"><span class="w-6" style="background:#555"></span><span class="w-2" style="background:#C0C0C0"></span><span class="w-10" style="background:#555"></span><span class="w-2" style="background:#C0C0C0"></span><span class="w-6" style="background:#555"></span></div>
</main>"""))

# 2. Memory Warning
reg("home_mem_warning", "内存不足", lambda: page_wrap(f"""
<main class="dotgrid px-4 pt-10 pb-4" style="min-height:800px">
<div class="flex items-center justify-center" style="height:400px">
<div class="border border-[#111] bg-white rounded-xl px-6 py-8 text-center" style="width:320px">
<div class="track-x text-[16px] font-bold">内 存 不 足</div>
<div class="f11 c555 mt-4 tracking-[0.1em]">请关闭部分应用后重试</div>
<div class="f11 c555 mt-2 tracking-[0.1em]">或重启设备</div>
<div class="flex gap-4 justify-center mt-6">
<div class="btn-dark" style="display:inline-block">取消</div>
<div class="btn-light" style="display:inline-block">重启</div>
</div>
</div></div>
</main>"""))

# 3. My Library
reg("my_library", "我的书架", lambda: page_wrap(header_bar("我 的 书 架") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("文件", list_item("小说/", "12 个文件", True) + list_item("历史/", "8 个文件") + list_item("科技/", "15 个文件") + list_item("哲学/", "5 个文件"))}
{divider()}
{section("书籍", list_item("三体：黑暗森林.epub", "28.5MB · 刘慈欣", True) + list_item("活着.txt", "512KB · 余华") + list_item("百年孤独.epub", "18.2MB · 马尔克斯") + list_item("人类简史.xtc", "24.1MB · 赫拉利") + list_item("道德经.txt", "128KB · 老子"))}
{footer(["返回", "打开", "上", "下"])}
</main>"""))

# 4. Recent Books
reg("recent_books", "最近阅读", lambda: page_wrap(header_bar("最 近 阅 读") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", list_item("三体", "75% · 刘慈欣", True) + list_item("活着", "32% · 余华") + list_item("百年孤独", "88% · 马尔克斯") + list_item("人类简史", "45% · 赫拉利") + list_item("道德经", "12% · 老子") + list_item("时间简史", "60% · 霍金") + list_item("围城", "3% · 钱钟书") + list_item("红楼梦", "28% · 曹雪芹"))}
{footer(["返回", "打开", "上", "下"])}
</main>"""))

# 5-7. Reader pages
def reader_page(title, author, pct, content_text, cover_key, chapter=""):
    status = f'<div class="px-4 mt-2 flex justify-between items-center text-[10px] text-[#555] tracking-[0.12em]"><span>{chapter}</span><span>{pct}%</span></div>' if chapter else f'<div class="px-4 mt-2 text-[10px] text-[#555] tracking-[0.12em]">{pct}%</div>'
    return page_wrap(header_bar("阅 读") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{status}
<div class="mt-2 px-4 text-[14px]" style="color:#111;line-height:1.85;text-indent:2em">
{content_text.replace(chr(10),"<br>")}
</div>
<div class="px-4 pb-3 mt-auto">
<div class="flex justify-center gap-2 mt-3"><span class="nav-item act">←</span><span class="nav-item ">→</span></div>
</div>
</main>""")

reg("reader_epub", "EPUB 阅读", lambda: reader_page("三 体", "刘慈欣", 75, "宇宙就是一座黑暗森林，每个文明都是带枪的猎人，像幽灵般潜行于林间，轻轻拨开挡路的树枝，竭力不让脚步发出一点儿声音，连呼吸都必须小心翼翼。\n\n他必须小心，因为林中到处都有与他一样潜行的猎人。如果他发现了别的生命，能做的只有一件事：开枪消灭之。在这片森林中，他人就是地狱，就是永恒的威胁。", "santi", "第 四 章 · 三 体 问 题"))
reg("reader_xtc", "XTC 阅读", lambda: reader_page("活 着", "余华", 32, "我比现在年轻十岁的时候，觉得活着就是活着，没有什么特别的意义。后来我才明白，活着本身就是最大的意义。\n\n人是为了活着本身而活着的，而不是为了活着之外的任何事物而活着。我认识一个老人，他叫福贵，他的一生，就是一本书。", "huozhe", "第 四 章 · 归 来"))
reg("reader_txt", "TXT 阅读", lambda: reader_page("道 德 经", "老子", 12, "道可道，非常道；名可名，非常名。\n无名天地之始，有名万物之母。\n故常无欲，以观其妙；常有欲，以观其徼。\n此两者同出而异名，同谓之玄。玄之又玄，众妙之门。", "daode", "第 一 章 · 道 可 道"))

# 8-9. Reader Menus
def reader_menu(title, items):
    rows = "".join(list_item(it, sel=(i==0)) for i,it in enumerate(items))
    return page_wrap(header_bar(title) + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("菜单", rows)}
{footer(["返回", "确认", "上", "下"])}
</main>""")

reg("reader_menu_epub", "EPUB 菜单", lambda: reader_menu("三 体", ["目录", "书签", "阅读设置", "字体设置", "搜索", "跳转到...", "分享"]))
reg("reader_menu_xtc", "XTC 菜单", lambda: reader_menu("活 着", ["目录", "书签管理", "阅读设置", "翻页设置", "进度跳转"]))

# 10-12. Chapters
def chapter_page(title, chapters):
    rows = "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{ch}</span><span class="f10 c888">{pg}</span></div>' for i,(ch,pg) in enumerate(chapters))
    return page_wrap(header_bar(title) + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("目录", rows)}
{footer(["返回", "跳转", "上", "下"])}
</main>""")

reg("chapter_epub", "EPUB 目录", lambda: chapter_page("目 录 · 三 体", [("第一章 科学边界","第1页"),("第二章 台球","第15页"),("第三章 射手和农场主","第28页"),("第四章 三体问题","第42页"),("第五章 红岸基地","第58页"),("第六章 叶文洁","第76页"),("第七章 三体游戏","第95页"),("第八章 面壁者","第120页")]))
reg("chapter_xtc", "XTC 目录", lambda: chapter_page("目 录 · 活 着", [("第一章 少年","第1页"),("第二章 败家","第23页"),("第三章 从军","第45页"),("第四章 归来","第67页"),("第五章 改革","第89页"),("第六章 暮年","第110页")]))
reg("chapter_txt", "TXT 目录", lambda: chapter_page("目 录 · 道 德 经", [("第一章 道可道","第1页"),("第二章 天下皆知","第3页"),("第三章 不尚贤","第5页"),("第四章 道冲而用之","第7页"),("第五章 天地不仁","第9页"),("第六章 谷神不死","第11页"),("第七章 天长地久","第13页"),("第八章 上善若水","第15页")]))

# 13. Reader Settings
reg("reader_settings", "阅读设置", lambda: page_wrap(header_bar("阅 读 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="border-line bg-card mx-4 mt-3 p-3 flex items-center gap-3">
<div class="pixel-cover" style="grid-template-columns:repeat(8,4px);grid-template-rows:repeat(10,4px)">{cover_for("santi", 8, 10)}</div>
<div class="flex-1"><div class="text-[13px] font-bold track-m">三 体</div><div class="text-[10px] text-[#555] mt-1">75% · 已读 245 页</div></div>
<div class="track-x text-[12px] font-bold">75%</div>
</div>
{settings_list([("字号","中"),("行间距","1.5倍"),("字间距","标准"),("上边距","12"),("下边距","12"),("左边距","8"),("右边距","8"),("阅读背景","纯白"),("首行缩进","已开启"),("对齐方式","左对齐")])}
{footer(["返回", "修改", "上", "下"])}
</main>"""))

# 14. Percent Selection
reg("percent_selection", "跳转选择", lambda: page_wrap(header_bar("跳 转 到 ...") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-8 px-4"><div class="f11 c555 mb-4">选择阅读进度</div>
<div class="mb-2">{progress_bar(45)}</div>
<div class="track-x text-[16px] font-bold mb-6">4 5 %</div>
{"".join(f'<div class="border-line bg-card px-4 py-3 mb-2 flex items-center justify-between"><span class="f11 b">第 {p}%</span><span class="f10 c888">跳转</span></div>' for p in [10,25,50,75,90])}
</div>
{footer(["返回", "跳转", "上", "下"])}
</main>"""))

# 15. Font Selection
reg("font_selection", "字体选择", lambda: page_wrap(header_bar("字 体 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{d}</span></div>' for i,(n,d) in enumerate([("默认字体","思源黑体 · 系统"),("宋体","宋体 · 衬线"),("楷体","楷体 · 书法"),("黑体","黑体 · 无衬线"),("圆体","圆体 · 柔和")])))}
{footer(["返回", "确认", "上", "下"])}
</main>"""))

# 16. Bookmark Manager
reg("bookmark_manager", "书签管理", lambda: page_wrap(header_bar("书 签 管 理") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="list-item {"sel" if i==0 else ""}"><div class="f11 b">{"" if i==0 else ""}{l}</div><div class="sub">{d}</div></div>' for i,(l,d) in enumerate([("第一章 科学边界","第8页 · 2026-08-20"),("第三章 射手和农场主","第32页 · 2026-08-21"),("第五章 红岸基地","第65页 · 2026-08-22"),("第七章 三体游戏","第98页 · 2026-08-23")])))}
{footer(["返回", "跳转", "删除", "新增"])}
</main>"""))

# 17. Bookmark Notes
reg("bookmark_notes", "书签笔记", lambda: page_wrap(header_bar("书 签 笔 记") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="list-item {"sel" if i==0 else ""}"><div class="f11 b">{"" if i==0 else ""}「{q}」</div><div class="sub">{s}</div></div>' for i,(q,s) in enumerate([("宇宙就是一座黑暗森林","三体 · 第45页"),("人是为了活着本身而活着的","活着 · 第32页"),("道可道，非常道","道德经 · 第1页"),("天下皆知美之为美","道德经 · 第3页")])))}
{footer(["返回", "编辑", "上", "下"])}
</main>"""))

# 18. Auto Page Turn
reg("auto_page_turn", "自动翻页间隔", lambda: page_wrap(header_bar("自 动 翻 页") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==1 else ""}"><span class="f11 b">{"" if i==1 else ""}{l}</span><span class="f10 c888">{d}</span></div>' for i,(l,d) in enumerate([("5秒","快速"),("10秒","推荐"),("15秒","适中"),("30秒","慢速"),("60秒","极慢")])))}
{footer(["返回", "确认", "上", "下"])}
</main>"""))

# 19. Tilt Page Turn
reg("tilt_page_turn", "倾斜翻页设置", lambda: page_wrap(header_bar("倾 斜 翻 页") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c555">{v}</span></div>' for i,(n,v) in enumerate([("启用倾斜翻页","已开启"),("灵敏度","中"),("倾斜角度","15°"),("方向","左倾/右倾")])))}
{footer(["返回", "切换", "上", "下"])}
</main>"""))

# 20. Settings Display
def settings_page(tab_active, items):
    return page_wrap(header_bar("系 统 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{tab_bar(["显示","按钮","系统"], tab_active)}
{settings_list(items)}
{footer(["返回", "修改", "上", "下"])}
</main>""")

reg("settings_display", "设置-显示", lambda: settings_page(0, [("锁屏壁纸","透明叠加"),("阅读进度","无进度"),("隐藏电池百分比","从不"),("刷新频率","5页全刷"),("永不全刷","已关闭"),("按钮提示","已开启"),("前光亮度","60"),("前光色温","50"),("关机前全刷","已开启"),("图片质量","普通"),("图标风格","风格1"),("图标选中风格","仅圆角"),("系统字号","中")]))

# 21. Settings Controls
reg("settings_controls", "设置-按钮", lambda: settings_page(1, [("映射侧边按钮","已开启"),("长按侧边按钮翻页","已开启"),("长按跳过章节","已开启"),("翻页键反转","已关闭"),("按键震动","已关闭"),("自定义按键映射","")]))

# 22. Settings System
reg("settings_system", "设置-系统", lambda: settings_page(2, [("系统语言","简体中文"),("开机自动同步时间","已开启"),("每次都重新选择WIFI","已关闭"),("直读TXT文档","已开启"),("休眠时间","10分钟"),("蓝牙设置","已关闭"),("坚果云配置",""),("数据胶囊配置",""),("SD卡升级",""),("还原为初始设置",""),("清理缓存",""),("切换启动区","APP0（官方）"),("开发者选项",""),("系统动画","已开启"),("动画步数","8"),("动画帧率","中(0x44)")]))

# 23. Button Remap
reg("button_remap", "按键映射", lambda: page_wrap(header_bar("按 键 映 射") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("上键","翻上页"),("下键","翻下页"),("左键","后退"),("右键","前进"),("确认键","确认"),("返回键","返回"),("侧边键1","目录"),("侧边键2","书签")])}
{footer(["返回", "修改", "上", "下"])}
</main>"""))

# 24. Calibre Settings
reg("calibre_settings", "Calibre 设置", lambda: page_wrap(header_bar("C a l i b r e") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("Calibre 服务器","192.168.1.100:8080"),("用户名","admin"),("密码","********"),("自动连接","已开启"),("无线设备名","Murphy M4")])}
{footer(["返回", "保存", "上", "下"])}
</main>"""))

# 25. Clear Cache
reg("clear_cache", "清理缓存", lambda: page_wrap(header_bar("清 理 缓 存") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4"><div class="f11 b mb-3">缓存占用</div>
{"".join(f'<div class="flex items-center justify-between list-item"><span class="f10 c555">{n}</span><span class="f10 c888">{s}</span></div>' for n,s in [("EPUB 缓存","128.5 MB"),("封面缩略图","45.2 MB"),("阅读进度","1.2 MB"),("书签数据","0.3 MB"),("临时文件","32.0 MB")])}
{divider()}
<div class="flex items-center justify-between list-item" style="border-color:#111"><span class="f11 b">总计</span><span class="f11 b">207.2 MB</span></div>
</div>
{footer(["返回", "全部清除", "上", "下"])}
</main>"""))

# 26. Data Capsule Settings
reg("data_capsule_settings", "数据胶囊配置", lambda: page_wrap(header_bar("数 据 胶 囊") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("服务器地址","dav.example.com"),("用户名","user@example.com"),("密码","********"),("同步目录","/Books/"),("自动同步","已开启"),("同步间隔","30分钟")])}
{footer(["返回", "保存", "上", "下"])}
</main>"""))

# 27. Developer Options
reg("developer_options", "开发者选项", lambda: page_wrap(header_bar("开 发 者 选 项") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("USB 调试","已开启"),("WiFi 调试","已关闭"),("日志输出级别","信息"),("屏幕刷新模式","快速"),("显示帧率","已关闭"),("强制全刷",""),("进入 DFU 模式",""),("运行测试","")])}
{footer(["返回", "确认", "上", "下"])}
</main>"""))

# 28. JianGuoYun Settings
reg("jianguo_yun_settings", "坚果云配置", lambda: page_wrap(header_bar("坚 果 云 配 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("坚果云账号","user@example.com"),("应用密码","********"),("同步文件夹","/我的书籍"),("自动同步","已开启"),("仅WiFi同步","已开启")])}
{footer(["返回", "保存", "上", "下"])}
</main>"""))

# 29. KOReader Auth
reg("koreader_auth", "KOReader 登录", lambda: page_wrap(header_bar("K O R e a d e r") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4"><div class="f10 c555 mb-4">请登录 KOReader 账号以同步阅读数据</div>
{settings_list([("用户名","reader@example.com"),("密码","********"),("服务器","https://sync.koreader.com")])}
<div class="mt-4 text-center"><div class="btn-dark" style="width:100%">登 录</div></div>
</div>
{footer(["返回", "登录", "上", "下"])}
</main>"""))

# 30. KOReader Settings
reg("koreader_settings", "KOReader 同步", lambda: page_wrap(header_bar("K O R e a d e r") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("自动同步","已开启"),("同步阅读进度","已开启"),("同步书签","已开启"),("文档匹配方式","文件名"),("最后同步","2026-08-27 10:30")])}
{footer(["返回", "同步", "上", "下"])}
</main>"""))

# 31. Number Selection
reg("number_selection", "数值选择", lambda: page_wrap(header_bar("数 值 选 择") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-8 px-4 text-center">
<div class="f11 c555 mb-6">选择数值</div>
<div class="inline-flex items-center justify-center border border-[#111] rounded-xl" style="width:80px;height:60px"><span class="track-x text-[18px] font-bold">12</span></div>
<div class="f10 c888 mt-4">最小值: 1&nbsp;&nbsp;&nbsp;最大值: 30&nbsp;&nbsp;&nbsp;步长: 1</div>
<div class="flex gap-4 mt-6 justify-center">
<div class="btn-light" style="display:inline-block;width:80px">－</div>
<div class="btn-dark" style="display:inline-block;width:80px">＋</div>
</div>
</div>
{footer(["返回", "确认", "", ""])}
</main>"""))

# 32. Online OTA
reg("online_ota", "在线更新", lambda: page_wrap(header_bar("在 线 更 新") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="flex items-center justify-between py-3"><span class="f11 b">当前版本</span><span class="f10 c555">v2.1.0 (build 20260801)</span></div>
<div class="flex items-center justify-between py-3"><span class="f11 b">最新版本</span><span class="f10 c111">v2.1.2 (build 20260820)</span></div>
{divider()}
<div class="f11 b mt-2 mb-3">更新内容</div>
{"".join(f'<div class="f10 c555 py-1">• {l}</div>' for l in ["修复部分EPUB图片显示问题","优化WiFi连接稳定性","新增自动翻页功能","改进电池续航管理"])}
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">检 查 更 新</div></div>
</div>
{footer(["返回", "更新", "", ""])}
</main>"""))

# 33. OTA Update Progress
reg("ota_update", "系统升级", lambda: page_wrap(header_bar("系 统 升 级") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-8 px-4 text-center">
<div class="f11 b mb-6">正在下载更新...</div>
{progress_bar(67)}
<div class="track-x text-[16px] font-bold mt-3">6 7 %</div>
<div class="f10 c555 mt-4">请勿关闭设备</div>
<div class="f10 c888 mt-2">正在下载: update-v2.1.2.bin (4.2MB/6.3MB)</div>
</div>
{footer(["", "", "", ""])}
</main>"""))

# 34. Reset Settings
reg("reset_settings", "还原设置", lambda: page_wrap(header_bar("还 原 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f11 b mb-2">此操作将还原所有设置为出厂状态。</div>
<div class="f11 b mt-4 mb-2">以下数据将被清除:</div>
{"".join(f'<div class="f10 c555 py-1">• {l}</div>' for l in ["所有系统设置","WiFi 密码","蓝牙配对信息","云服务账号","阅读偏好设置"])}
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">还 原 设 置</div></div>
</div>
{footer(["返回", "确认", "", ""])}
</main>"""))

# 35. Simple Bluetooth
reg("simple_bluetooth", "蓝牙设置", lambda: page_wrap(header_bar("蓝 牙 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">蓝牙</div>
<div class="flex items-center justify-between border-line bg-card px-4 py-3"><span class="f11 b">启用蓝牙</span><span class="f10 c111 border border-[#111] rounded-full px-3 py-0.5">开</span></div>
{divider()}
<div class="f11 b mt-2 mb-2">已配对设备</div>
{"".join(f'<div class="flex items-center justify-between py-2"><span class="f11">{n}</span><span class="f10 c888">{s}</span></div>' for n,s in [("蓝牙键盘","已连接"),("蓝牙耳机","未连接"),("手机 - Murphy","已配对")])}
</div>
{footer(["返回", "扫描", "上", "下"])}
</main>"""))

# 36. App List
reg("app_list", "应用列表", lambda: page_wrap(header_bar("应 用") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{d}</span></div>' for i,(n,d) in enumerate([("KOReader 同步","数据同步"),("晋江云同步","云存储"),("Calibre 传输","无线传书"),("OPDS 浏览器","在线书库"),("数据胶囊","WebDAV"),("屏幕桥接","远程控制"),("文件管理器","系统工具")])))}
{footer(["返回", "卸载", "上", "下"])}
</main>"""))

# 37. App Install
reg("app_install", "安装扩展", lambda: page_wrap(header_bar("安 装 扩 展") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="list-item {"sel" if i==0 else ""}"><div class="flex items-center justify-between"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c111">安装</span></div><div class="sub">{d}</div></div>' for i,(n,d) in enumerate([("ko-sync-v2.mpk","KOReader 同步 v2.0"),("jianguo.mpk","晋江云同步 v1.3"),("calibre-wireless.mpk","Calibre 无线传书 v1.1"),("opds-browser.mpk","OPDS 浏览器 v2.0")])))}
{footer(["返回", "安装", "上", "下"])}
</main>"""))

# 38. App Runtime
reg("app_runtime", "应用运行时", lambda: page_wrap(header_bar("应 用 运 行 时") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">正在运行的应用</div>
{"".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{s}</span></div>' for i,(n,s) in enumerate([("KOReader 同步","后台运行中"),("晋江云同步","空闲")]))}
{divider()}
<div class="f10 c555 mt-2">内存使用: 12.5 MB / 64 MB</div>
</div>
{footer(["返回", "停止", "上", "下"])}
</main>"""))

# 39. Native App
reg("native_app", "原生应用", lambda: page_wrap(header_bar("原 生 应 用") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{v}</span></div>' for i,(n,v) in enumerate([("EPUB 阅读器","版本 2.1.0"),("XTC 阅读器","版本 2.1.0"),("TXT 阅读器","版本 2.1.0"),("我的书架","版本 2.1.0"),("系统设置","版本 2.1.0")])))}
{footer(["返回", "打开", "上", "下"])}
</main>"""))

# 40. Native Provider Book
reg("native_provider_book", "内容提供商", lambda: page_wrap(header_bar("内 容 提 供 商") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{d}</span></div>' for i,(n,d) in enumerate([("本地文件","SD 卡书籍"),("KOReader","同步书籍"),("晋江云","云存储书籍"),("Calibre","无线传书"),("OPDS","在线书库"),("数据胶囊","WebDAV 书籍")])))}
{footer(["返回", "选择", "上", "下"])}
</main>"""))

# 41. Native Provider Login
reg("native_provider_login", "登录", lambda: page_wrap(header_bar("登 录") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f10 c555 mb-4">请登录以访问内容提供商</div>
{settings_list([("服务器地址","example.com"),("用户名","user"),("密码","********")])}
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">登 录</div></div>
</div>
{footer(["返回", "登录", "", ""])}
</main>"""))

# 42. Native Provider Endpoint
reg("native_provider_endpoint", "端点配置", lambda: page_wrap(header_bar("端 点 配 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("端点 URL","https://api.example.com/v1"),("API Key","sk-xxxxxxxxxxxx"),("超时时间","30秒"),("重试次数","3")])}
{footer(["返回", "测试", "上", "下"])}
</main>"""))

# 43. Screen Bridge
reg("screen_bridge", "屏幕桥接", lambda: page_wrap(header_bar("屏 幕 桥 接") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f10 c555 mb-4">将设备屏幕投射到浏览器</div>
<div class="space-y-3">
<div class="flex items-center"><span class="f11 b" style="width:80px">状态:</span><span class="f11">运行中</span></div>
<div class="flex items-center"><span class="f11 b" style="width:80px">地址:</span><span class="f11">http://192.168.1.100:8080</span></div>
<div class="flex items-center"><span class="f11 b" style="width:80px">连接设备:</span><span class="f11">1</span></div>
</div>
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">停 止 桥 接</div></div>
</div>
{footer(["返回", "刷新", "", ""])}
</main>"""))

# 44. WiFi Selection
reg("wifi_selection", "Wi-Fi 设置", lambda: page_wrap(header_bar("W i - F i 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">Wi-Fi</div>
<div class="flex items-center justify-between border-line bg-card px-4 py-3"><span class="f11 b">启用 Wi-Fi</span><span class="f10 c111 border border-[#111] rounded-full px-3 py-0.5">开</span></div>
{divider()}
<div class="f11 b mt-2 mb-2">可用网络</div>
{"".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{s}</span></div>' for i,(n,s) in enumerate([("Home-WiFi-5G","已连接"),("Office-Guest","信号强"),("CoffeeShop-Free","开放"),("Neighbor-2.4G","信号弱")]))}
</div>
{footer(["返回", "连接", "刷新"])}
</main>"""))

# 45. Network Mode
reg("network_mode", "网络模式", lambda: page_wrap(header_bar("网 络 模 式") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-3">你想如何连接?</div>
{"".join(f'<div class="list-item {"sel" if i==0 else ""}"><div class="f11 b">{"" if i==0 else ""}{n}</div><div class="sub">{d}</div></div>' for i,(n,d) in enumerate([("1) 手机连接到设备传书（推荐）","通过设备热点直连传书"),("2) 设备连接到WiFi传书","设备连接路由器后传书"),("3) 使用Calibre无线设备传输","通过Calibre无线传书")]))}
</div>
{footer(["返回", "继续", "上", "下"])}
</main>"""))

# 46. Calibre Connect
reg("calibre_connect", "Calibre 连接", lambda: page_wrap(header_bar("C a l i b r e") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f10 c555 mb-4">通过 Calibre 无线传输书籍</div>
<div class="space-y-3">
<div class="flex items-center"><span class="f11 b" style="width:100px">状态:</span><span class="f11">等待连接</span></div>
<div class="flex items-center"><span class="f11 b" style="width:100px">服务器地址:</span><span class="f11">192.168.1.100:8080</span></div>
<div class="flex items-center"><span class="f11 b" style="width:100px">设备名称:</span><span class="f11">Murphy M4</span></div>
</div>
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">开 始 连 接</div></div>
</div>
{footer(["返回", "刷新", "", ""])}
</main>"""))

# 47. CrossPoint Web Server
reg("cross_point_web_server", "Web 服务器", lambda: page_wrap(header_bar("W e b 服 务 器") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f10 c555 mb-4">通过浏览器管理设备</div>
<div class="space-y-3">
<div class="flex items-center"><span class="f11 b" style="width:80px">状态:</span><span class="f11">运行中</span></div>
<div class="flex items-center"><span class="f11 b" style="width:80px">地址:</span><span class="f11">http://192.168.1.100</span></div>
<div class="flex items-center"><span class="f11 b" style="width:80px">端口:</span><span class="f11">80</span></div>
</div>
<div class="mt-6 text-center"><div class="btn-dark" style="width:100%">停 止 服 务 器</div></div>
</div>
{footer(["返回", "打开", "", ""])}
</main>"""))

# 48. OPDS Browser
reg("opds_browser", "OPDS 书库", lambda: page_wrap(header_bar("O P D S 书 库") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">服务器</div>
<div class="border-line bg-card px-4 py-3"><span class="f10 c555">https://opds.example.com/opds</span></div>
{divider()}
{"".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{c}</span><span class="f10 c888">{n}</span></div>' for i,(c,n) in enumerate([("最新上架","120 本"),("热门推荐","85 本"),("经典文学","230 本"),("科学技术","156 本"),("哲学思想","78 本"),("历史传记","92 本")]))}
</div>
{footer(["返回", "浏览", "上", "下"])}
</main>"""))

# 49. JianGuo Browser
reg("jianguo_browser", "坚果云浏览", lambda: page_wrap(header_bar("坚 果 云") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">我的文件</div>
{"".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{s}</span></div>' for i,(n,s) in enumerate([("小说/","12 本"),("历史/","5 本"),("科技/","8 本"),("三体.epub","28.5MB"),("活着.txt","512KB"),("百年孤独.epub","18.2MB"),("人类简史.xtc","24.1MB")]))}
</div>
{footer(["返回", "下载", "上", "下"])}
</main>"""))

# 50. Data Capsule Browser
reg("data_capsule_browser", "数据胶囊浏览", lambda: page_wrap(header_bar("数 据 胶 囊") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-3 px-4">
<div class="f11 b mb-2">我的文件</div>
{"".join(f'<div class="flex items-center justify-between list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{n}</span><span class="f10 c888">{s}</span></div>' for i,(n,s) in enumerate([("Documents/","15 个文件"),("Books/","23 个文件"),("report.pdf","2.5MB"),("notes.txt","128KB"),("data.json","512KB")]))}
</div>
{footer(["返回", "下载", "上", "下"])}
</main>"""))

# 51. Boot Screen
reg("boot", "开机", lambda: f"""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=400"><title>Murphy M4 Boot</title><style>*{{margin:0;padding:0;box-sizing:border-box}}body{{background:#000;font-family:'Courier New',Courier,monospace;display:flex;justify-content:center;padding:20px 0}}.page{{width:400px;height:800px;background:#000;display:flex;flex-direction:column;align-items:center;justify-content:center;overflow:hidden}}p{{height:3px;border-radius:2px;background:#444;width:192px}}p>span{{display:block;height:3px;border-radius:2px;background:#fff;width:45%}}.text-white{{color:#fff}}.text-\[#888\]{{color:#888}}.text-\[22px\]{{font-size:22px}}.text-\[12px\]{{font-size:12px}}.text-\[10px\]{{font-size:10px}}.text-\[9px\]{{font-size:9px}}.font-bold{{font-weight:bold}}.mt-1{{margin-top:4px}}.mt-4{{margin-top:16px}}.mt-6{{margin-top:24px}}.mt-8{{margin-top:32px}}.tracking-\[0\.12em\]{{letter-spacing:0.12em}}.tracking-\[0\.15em\]{{letter-spacing:0.15em}}.tracking-\[0\.1em\]{{letter-spacing:0.1em}}</style></head><body><div class="page">
<div class="text-[22px] font-bold tracking-[0.12em] text-white">墨菲M4</div>
<div class="text-[9px] text-white mt-1 tracking-[0.15em]">CrossPoint</div>
<div class="text-[12px] text-[#888] mt-4 tracking-[0.15em]">正在启动...</div>
<div class="text-[10px] text-[#888] mt-6 tracking-[0.1em]">v2.1.0 (build 20260801)</div>
<div class="mt-8"><p><span></span></p></div>
</div></body></html>""")

# 52. Sleep Screen
reg("sleep", "休眠", lambda: """<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=400"><title>Murphy M4 Sleep</title><style>*{margin:0;padding:0;box-sizing:border-box}body{background:#000;font-family:'Courier New',Courier,monospace;display:flex;justify-content:center;padding:20px 0}.page{width:400px;height:800px;background:#000;display:flex;flex-direction:column;align-items:center;justify-content:center;overflow:hidden}.text-white{color:#fff}.text-\[#888\]{color:#888}.text-\[22px\]{font-size:22px}.text-\[12px\]{font-size:12px}.font-bold{font-weight:bold}.mt-4{margin-top:16px}.tracking-\[0\.3em\]{letter-spacing:0.3em}.tracking-\[0\.15em\]{letter-spacing:0.15em}</style></head><body><div class="page">
<div class="text-[22px] font-bold tracking-[0.3em] text-white">已 休 眠</div>
<div class="text-[12px] text-[#888] mt-4 tracking-[0.15em]">轻触唤醒</div>
</div></body></html>""")

# 53. Full Screen Message
reg("full_screen_message", "全屏消息", lambda: page_wrap(f"""
<main class="dotgrid px-4 pb-4" style="min-height:800px;display:flex;flex-direction:column;align-items:center;justify-content:center">
<div class="track-x text-[22px] font-bold">消 息</div>
<div class="f11 c555 mt-4 tracking-[0.1em]">操作已完成</div>
<div class="f10 c888 mt-6 tracking-[0.1em]">长按返回</div>
</main>"""))

# 54. Keyboard Entry
reg("keyboard_entry", "键盘输入", lambda: page_wrap(header_bar("输 入") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="border-line bg-card px-4 py-3"><span class="f10 c888">搜索书籍...</span></div>
<div class="mt-6 p-4 text-center" style="font-family:'Courier New',monospace;font-size:11px;letter-spacing:0.15em;border:2px solid #111;border-radius:4px;background:#fff">
<div class="mb-2">q w e r t y u i o p</div>
<div class="mb-2 ml-6">a s d f g h j k l</div>
<div class="mb-2 ml-4">z x c v b n m 删除</div>
<div class="mt-3">123&nbsp;&nbsp;&nbsp;&nbsp;空格&nbsp;&nbsp;&nbsp;&nbsp;确认</div>
</div>
</div>
{footer(["返回", "输入", "", ""])}
</main>"""))

# 55. JianGuo Sync
reg("jianguo_sync", "晋江云同步", lambda: page_wrap(header_bar("晋 江 云 同 步") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f11 b mb-3">同步状态</div>
<div class="f10 c555 mb-3">最后同步: 2026-08-27 10:30</div>
<div class="f11 mb-2">同步中...</div>
{progress_bar(55)}
<div class="f10 c888 mt-2">正在同步: 三体.epub (12.5MB/28.5MB)</div>
{divider()}
<div class="f11 b mt-2 mb-2">同步队列</div>
{"".join(f'<div class="list-item f10 c555">{i+1}. {item}</div>' for i,item in enumerate(["三体.epub","活着.txt","百年孤独.epub","人类简史.xtc"]))}
</div>
{footer(["返回", "暂停", "", ""])}
</main>"""))

# 56. KOReader Sync
reg("koreader_sync", "KOReader 同步", lambda: page_wrap(header_bar("K O R e a d e r") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
<div class="pt-4 px-4">
<div class="f11 b mb-3">同步状态</div>
<div class="f10 c555 mb-2">已连接: reader@example.com</div>
<div class="f10 c555 mb-3">最后同步: 2026-08-27 10:30</div>
<div class="f11 mb-2">同步进度</div>
{progress_bar(100)}
<div class="mt-3">{"".join(f'<div class="list-item f10 c555">✓ {l}</div>' for l in ["阅读进度已同步","书签已同步","阅读设置已同步"])}</div>
</div>
{footer(["返回", "立即同步", "", ""])}
</main>"""))

# 57. Epub Reader Settings
reg("epub_reader_settings", "EPUB 阅读设置", lambda: page_wrap(header_bar("E P U B 设 置") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{settings_list([("显示时间","已开启"),("显示EPUB图片","已开启"),("标点宽度","标准"),("下划线","已关闭"),("段落间距","标准"),("侧边按钮设置","仅阅读")])}
{footer(["返回", "修改", "上", "下"])}
</main>"""))

# 58. Activity With Sub
reg("activity_with_sub", "子页面", lambda: page_wrap(header_bar("子 页 面") + f"""
<main class="dotgrid px-4 pb-4" style="min-height:calc(800px-84px);display:flex;flex-direction:column">
{section("", "".join(f'<div class="list-item {"sel" if i==0 else ""}"><span class="f11 b">{"" if i==0 else ""}{item}</span></div>' for i,item in enumerate(["子页面 1","子页面 2","子页面 3","子页面 4","返回主页面"])))}
{footer(["返回", "确认", "上", "下"])}
</main>"""))

# === GENERATE ALL ===

def generate_all():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"生成 {len(PAGES)} 个像素点阵风格页面 → {OUT}")
    for name, label, fn in PAGES:
        html = fn()
        (OUT / f"{name}.html").write_text(html, encoding="utf-8")
        print(f"  ✓ {label} ({name}.html)")

    cards = "".join(f'<div class="card"><div class="lbl">{l}</div><div class="fn">{n}.html</div><a href="{n}.html"><iframe src="{n}.html" style="width:100%;height:160px;border:1px solid #C0C0C0;background:#fff"></iframe></a></div>' for n,l,_ in PAGES)
    index_html = f"""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=400"><title>Murphy M4 像素点阵 UI 预览</title>
<style>*{{margin:0;padding:0;box-sizing:border-box}}body{{font-family:'Courier New',Courier,monospace;background:#fff;padding:24px;color:#111}}h1{{font-size:20px;letter-spacing:0.1em;margin-bottom:4px}}p.desc{{color:#111;font-size:11px;margin-bottom:20px;letter-spacing:0.1em}}.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:10px}}.card{{background:#fff;border:1px solid #C0C0C0;padding:8px}}.card .lbl{{font-size:11px;font-weight:bold;letter-spacing:0.08em;margin-bottom:2px}}.card .fn{{color:#111;font-size:9px;margin-bottom:4px;letter-spacing:0.05em}}.card a{{display:block}}.card img{{width:100%;border:1px solid #C0C0C0;image-rendering:pixelated;display:block;min-height:100px;background:#fff}}</style></head><body>
<h1 style="letter-spacing:0.12em">墨菲M4</h1>
<p class="desc">像素点阵风格 · {len(PAGES)} 个屏幕 · 400×800</p>
<div class="grid">{cards}</div></body></html>"""
    (OUT / "index.html").write_text(index_html, encoding="utf-8")
    print(f"  ✓ 索引页 (index.html)")
    # 用已确认的自包含定稿覆盖 home（保证 brand/标题/纯黑一致，且不依赖 Tailwind CDN）
    draft_home = Path(__file__).resolve().parent / "preview_assets" / "pixel-dot-grid-home.html"
    if draft_home.exists():
        import shutil
        shutil.copy(draft_home, OUT / "home.html")
        print("  ✓ 主页已用确认版定稿覆盖 (home.html, 自包含无CDN)")
    print(f"完成！打开 {OUT / 'index.html'} 查看全部页面")

if __name__ == "__main__":
    generate_all()
