#!/usr/bin/env python3
"""Render real Lua gui-command traces into 1-bit UI previews (NOT hardware proof)."""
import argparse
import subprocess
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

BASE=Path(__file__).resolve().parent
ROOT=BASE.parent
FONT_CANDIDATES=[
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]
def font(size):
    for path in FONT_CANDIDATES:
        try: return ImageFont.truetype(path,size=size)
        except OSError: pass
    return ImageFont.load_default()

def frames(lines):
    name,img,drawing=None,None,None
    for raw in lines:
        line=raw.strip("\r\n")
        if line.startswith("FRAME|"):
            if name is not None: yield name,img
            name=line.split("|",1)[1]
            img=Image.new("1",(480,800),1)
            drawing=ImageDraw.Draw(img)
            continue
        if name is None: continue
        parts=line.split("|",4)
        typ=parts[0]
        if typ=="C":
            drawing.rectangle((0,0,479,799),fill=1)
        elif typ in ("R","F"):
            _,x,y,w,h=parts
            x,y,w,h=map(int,(x,y,w,h))
            bbox=(x,y,x+w-1,y+h-1)
            if typ=="F":drawing.rectangle(bbox,fill=0)
            else:drawing.rectangle(bbox,outline=0,width=1)
        elif typ=="L":
            _,a,b,c,d=parts
            drawing.line(tuple(map(int,(a,b,c,d))),fill=0,width=1)
        elif typ=="T":
            _,size,x,y,text=parts
            drawing.text((int(x),int(y)),text,font=font(int(size)),fill=0)
        elif line=="TRACE_END":
            break
    if name is not None: yield name,img

def main():
    a=argparse.ArgumentParser()
    a.add_argument("--host",required=True)
    a.add_argument("--out",type=Path,default=ROOT/"tools"/"previews")
    opts=a.parse_args()
    process=subprocess.run([opts.host,str(BASE/"render_ui_trace.lua")],
                           cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                           encoding="utf-8",check=True)
    opts.out.mkdir(parents=True,exist_ok=True)
    outputs=dict(frames(process.stdout.splitlines()))
    expected={"picker","game","help","win"}
    assert set(outputs)==expected,(set(outputs),process.stderr)
    for title,picture in outputs.items():
        picture.save(opts.out/f"sokoban_{title}_480x800.png")
    sheet=Image.new("1",(992,1654),1)
    draw=ImageDraw.Draw(sheet)
    for i,name in enumerate(("game","picker","win","help")):
        x=12+(i%2)*496
        y=22+(i//2)*818
        draw.text((x,y-20),name.upper(),font=font(15),fill=0)
        sheet.paste(outputs[name],(x,y))
    sheet.save(opts.out/"sokoban_four_screens_contact.png")
    print("RENDER_OK",", ".join(f"{k}=480x800" for k in outputs))

if __name__=="__main__": main()
