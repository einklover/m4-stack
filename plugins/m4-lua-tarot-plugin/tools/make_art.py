#!/usr/bin/env python3
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import math, struct

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'art'
W, H = 140, 240
BMP_W, BMP_H = 112, 192
CARDS = [
 ('fool','0'),('magician','I'),('priestess','II'),('empress','III'),('emperor','IV'),('hierophant','V'),('lovers','VI'),('chariot','VII'),('strength','VIII'),('hermit','IX'),('wheel','X'),('justice','XI'),('hanged','XII'),('death','XIII'),('temperance','XIV'),('devil','XV'),('tower','XVI'),('star','XVII'),('moon','XVIII'),('sun','XIX'),('judgement','XX'),('world','XXI')
]

try:
    FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 12)
    FONT_SMALL = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 9)
except Exception:
    FONT = ImageFont.load_default(); FONT_SMALL = FONT

def bits_from(im):
    g = im.convert('L')
    p = g.load()
    return [[1 if p[x,y] < 128 else 0 for x in range(g.width)] for y in range(g.height)]

def runtime_bits(im):
    return bits_from(im.resize((BMP_W, BMP_H), Image.Resampling.LANCZOS))

def write_bmp(path, bits, invert=False):
    h, w = len(bits), len(bits[0])
    row_bytes = (w + 31)//32*4
    image_size = row_bytes*h
    off = 62
    header = bytearray(off)
    header[:2] = b'BM'
    struct.pack_into('<I', header, 2, off+image_size)
    struct.pack_into('<I', header, 10, off)
    struct.pack_into('<I', header, 14, 40)
    struct.pack_into('<iiHHII', header, 18, w, h, 1, 1, 0, image_size)
    struct.pack_into('<I', header, 46, 2)
    header[54:58] = bytes((255,255,255,0))
    header[58:62] = bytes((0,0,0,0))
    body=bytearray()
    for y in range(h-1,-1,-1):
        row=bytearray(row_bytes)
        for x,v in enumerate(bits[y]):
            bit = 1-v if invert else v
            if bit: row[x>>3] |= 0x80>>(x&7)
        body.extend(row)
    path.write_bytes(header+body)

def star(draw,cx,cy,r1,r2,n=5,width=2):
    pts=[]
    for i in range(n*2):
        a=-math.pi/2+i*math.pi/n; r=r1 if i%2==0 else r2
        pts.append((cx+math.cos(a)*r, cy+math.sin(a)*r))
    draw.line(pts+[pts[0]], fill=0, width=width)

def sun(draw,cx,cy,r=24):
    draw.ellipse((cx-r,cy-r,cx+r,cy+r),outline=0,width=3)
    for i in range(12):
        a=i*math.pi/6
        draw.line((cx+math.cos(a)*(r+6),cy+math.sin(a)*(r+6),cx+math.cos(a)*(r+24),cy+math.sin(a)*(r+24)),fill=0,width=2)

def moon(draw,cx,cy,r=28):
    draw.ellipse((cx-r,cy-r,cx+r,cy+r),outline=0,width=3)
    draw.ellipse((cx-r+13,cy-r-2,cx+r+10,cy+r-2),fill=255,outline=255)

def human(draw,cx,cy,scale=1):
    rr=int(8*scale); draw.ellipse((cx-rr,cy-rr,cx+rr,cy+rr),outline=0,width=max(1,int(2*scale)))
    draw.line((cx,cy+rr,cx,cy+42*scale),fill=0,width=max(1,int(3*scale)))
    draw.line((cx,cy+18*scale,cx-20*scale,cy+32*scale),fill=0,width=max(1,int(2*scale)))
    draw.line((cx,cy+18*scale,cx+20*scale,cy+32*scale),fill=0,width=max(1,int(2*scale)))
    draw.line((cx,cy+42*scale,cx-16*scale,cy+66*scale),fill=0,width=max(1,int(2*scale)))
    draw.line((cx,cy+42*scale,cx+16*scale,cy+66*scale),fill=0,width=max(1,int(2*scale)))

def common(draw, roman):
    draw.rounded_rectangle((3,3,W-4,H-4),radius=5,outline=0,width=2)
    draw.rectangle((8,8,W-9,H-9),outline=0,width=1)
    box=draw.textbbox((0,0),roman,font=FONT); tw=box[2]-box[0]
    draw.text(((W-tw)//2,13),roman,font=FONT,fill=0)
    draw.line((18,34,W-18,34),fill=0,width=1)
    draw.line((18,H-30,W-18,H-30),fill=0,width=1)
    # four corner stars
    for x,y in ((15,18),(W-15,18),(15,H-16),(W-15,H-16)):
        draw.ellipse((x-1,y-1,x+1,y+1),fill=0)

def draw_symbol(im, idx):
    d=ImageDraw.Draw(im); cx=W//2
    if idx==0: # fool - cliff/wanderer
        human(d,62,75,.8); d.line((15,178,110,150,130,180),fill=0,width=3); d.ellipse((90,82,100,92),outline=0,width=2)
    elif idx==1: # magician
        human(d,cx,70,.8); d.text((64,52),'∞',font=FONT,fill=0); d.line((28,160,112,160),fill=0,width=2); d.ellipse((33,145,47,159),outline=0); d.rectangle((64,145,76,159),outline=0); star(d,99,152,8,3)
    elif idx==2:
        d.arc((34,62,106,134),0,180,fill=0,width=3); d.line((42,95,42,176),fill=0,width=5); d.line((98,95,98,176),fill=0,width=5); human(d,cx,88,.7); moon(d,cx,62,15)
    elif idx==3:
        d.ellipse((39,65,101,125),outline=0,width=3); d.arc((28,93,112,183),190,350,fill=0,width=3); d.line((55,124,48,174),fill=0,width=3); d.line((85,124,92,174),fill=0,width=3); star(d,70,95,12,5,6)
    elif idx==4:
        d.rectangle((36,92,104,173),outline=0,width=4); d.rectangle((29,82,45,173),outline=0,width=3); d.rectangle((95,82,111,173),outline=0,width=3); d.polygon([(44,89),(54,62),(65,89),(75,58),(88,89),(98,62),(104,89)],outline=0)
    elif idx==5:
        d.line((40,66,40,176),fill=0,width=5); d.line((100,66,100,176),fill=0,width=5); human(d,cx,88,.65); d.line((55,65,85,65),fill=0,width=3); d.line((70,50,70,78),fill=0,width=3)
    elif idx==6:
        human(d,47,105,.55); human(d,93,105,.55); d.line((47,82,70,62,93,82),fill=0,width=2); d.ellipse((60,48,80,68),outline=0,width=2); d.arc((32,148,108,198),190,350,fill=0,width=2)
    elif idx==7:
        d.rectangle((32,105,108,165),outline=0,width=4); d.ellipse((34,160,56,182),outline=0,width=3); d.ellipse((84,160,106,182),outline=0,width=3); human(d,cx,68,.6); d.line((36,103,22,77),fill=0,width=3); d.line((104,103,118,77),fill=0,width=3)
    elif idx==8:
        human(d,54,86,.6); d.ellipse((66,119,111,160),outline=0,width=3); d.arc((71,126,104,151),170,350,fill=0,width=2); d.line((84,145,92,160),fill=0,width=2); d.text((62,50),'∞',font=FONT,fill=0)
    elif idx==9:
        human(d,66,75,.65); d.line((91,96,91,180),fill=0,width=3); d.line((84,180,98,180),fill=0,width=2); d.rectangle((26,74,46,100),outline=0,width=3); star(d,36,86,7,3)
    elif idx==10:
        d.ellipse((30,64,110,144),outline=0,width=4); d.ellipse((48,82,92,126),outline=0,width=2); star(d,cx,104,19,8,8); d.line((70,48,70,160),fill=0,width=2); d.line((14,104,126,104),fill=0,width=2)
    elif idx==11:
        human(d,cx,72,.58); d.line((70,103,70,178),fill=0,width=3); d.line((37,119,103,119),fill=0,width=2); d.line((45,119,32,150,58,150,45,119),fill=0,width=2); d.line((95,119,82,150,108,150,95,119),fill=0,width=2); d.line((92,66,104,116),fill=0,width=3)
    elif idx==12:
        d.line((28,58,112,58),fill=0,width=4); d.line((70,58,70,88),fill=0,width=3); d.ellipse((62,84,78,100),outline=0,width=2); d.line((70,100,70,150),fill=0,width=3); d.line((70,118,48,137),fill=0,width=2); d.line((70,118,92,137),fill=0,width=2); d.line((70,150,51,177),fill=0,width=2); d.line((70,150,89,177),fill=0,width=2)
    elif idx==13:
        d.line((29,168,112,78),fill=0,width=4); d.arc((30,65,94,141),80,270,fill=0,width=4); d.line((48,70,96,70),fill=0,width=3); d.ellipse((42,102,62,122),outline=0,width=2); d.line((52,122,52,166),fill=0,width=3); d.line((41,143,64,143),fill=0,width=2)
    elif idx==14:
        human(d,cx,65,.6); d.arc((26,125,74,175),210,340,fill=0,width=3); d.arc((66,135,114,185),20,160,fill=0,width=3); d.line((54,126,87,164),fill=0,width=2); d.line((86,136,54,174),fill=0,width=2)
    elif idx==15:
        d.ellipse((52,60,88,96),outline=0,width=3); d.line((56,63,42,48),fill=0,width=3); d.line((84,63,98,48),fill=0,width=3); d.line((70,96,70,150),fill=0,width=4); d.line((70,118,42,140),fill=0,width=3); d.line((70,118,98,140),fill=0,width=3); d.arc((28,141,66,183),20,210,fill=0,width=2); d.arc((74,141,112,183),-30,160,fill=0,width=2)
    elif idx==16:
        d.polygon([(42,78),(98,78),(91,180),(49,180)],outline=0); d.rectangle((55,94,67,112),outline=0); d.rectangle((76,94,88,112),outline=0); d.line((47,78,54,58,64,78,72,52,81,78,89,60,95,78),fill=0,width=2); d.line((92,44,65,95,82,95,58,154),fill=0,width=5)
    elif idx==17:
        star(d,cx,92,30,13,8,3); [star(d,x,y,7,3,5,1) for x,y in ((32,68),(108,65),(29,137),(112,139),(48,165),(92,168))]; d.line((25,190,115,190),fill=0,width=2)
    elif idx==18:
        moon(d,cx,82,32); d.line((30,150,55,132,70,151,87,130,111,150),fill=0,width=2); d.ellipse((28,159,44,175),outline=0); d.ellipse((96,159,112,175),outline=0); d.line((70,127,70,190),fill=0,width=1)
    elif idx==19:
        sun(d,cx,90,27); d.line((22,164,118,164),fill=0,width=2); d.arc((34,130,106,191),180,360,fill=0,width=3); d.line((48,163,48,188),fill=0,width=2); d.line((92,163,92,188),fill=0,width=2)
    elif idx==20:
        d.arc((22,55,118,110),200,340,fill=0,width=4); d.line((34,84,70,58,106,84),fill=0,width=2); d.line((70,60,70,107),fill=0,width=2); [human(d,x,126,.42) for x in (40,70,100)]
    elif idx==21:
        d.ellipse((28,55,112,179),outline=0,width=3); d.arc((20,48,72,110),80,280,fill=0,width=2); d.arc((68,48,120,110),-100,100,fill=0,width=2); human(d,cx,88,.55); star(d,cx,68,12,5,6,2)

def make_card(idx, key, roman):
    im=Image.new('L',(W,H),255); d=ImageDraw.Draw(im); common(d,roman); draw_symbol(im,idx)
    # bottom key label is ASCII and remains legible on 1-bit output
    label=key.upper(); box=d.textbbox((0,0),label,font=FONT_SMALL); tw=box[2]-box[0]
    d.text(((W-tw)//2,H-24),label,font=FONT_SMALL,fill=0)
    return im

def make_back():
    im=Image.new('L',(W,H),255); d=ImageDraw.Draw(im)
    d.rounded_rectangle((3,3,W-4,H-4),radius=5,outline=0,width=3); d.rectangle((10,10,W-11,H-11),outline=0,width=2)
    for r in (20,36,52): d.ellipse((W//2-r,H//2-r,W//2+r,H//2+r),outline=0,width=2)
    star(d,W//2,H//2,43,18,8,2); moon(d,W//2,H//2,15)
    for y in range(30,H-30,24):
        d.line((20,y,W-20,H-y),fill=0,width=1)
    return im

def make_icon(back):
    # drawer icon: a tarot card with moon + star, then pre-invert for drawer path.
    im=Image.new('L',(62,64),255); d=ImageDraw.Draw(im)
    d.rounded_rectangle((8,2,53,61),radius=5,outline=0,width=3); d.rectangle((13,7,48,56),outline=0,width=1)
    moon(d,31,27,10); star(d,31,45,7,3,5,2)
    return im

def main():
    ART.mkdir(parents=True,exist_ok=True)
    ims=[]
    back=make_back(); write_bmp(ART/'card_back.bmp',runtime_bits(back))
    for idx,(key,roman) in enumerate(CARDS):
        im=make_card(idx,key,roman); write_bmp(ART/f'{idx:02d}_{key}.bmp',runtime_bits(im)); ims.append((f'{idx:02d} {key}',im))
    icon=make_icon(back); write_bmp(ROOT/'icon_home.bmp',bits_from(icon),invert=True)
    # One review sheet contains the app icon, card back, and all 22 actual card faces.
    icon_cell=Image.new('L',(W,H),255); scaled=icon.resize((124,128),Image.Resampling.NEAREST); icon_cell.paste(scaled,((W-124)//2,50))
    ims.insert(0,('ICON',icon_cell)); ims.insert(1,('BACK',back))
    # actual in-game asset contact sheet (not shipped by manifest)
    cols=5; cellw=158; cellh=276; rows=math.ceil(len(ims)/cols)
    sheet=Image.new('L',(cols*cellw,rows*cellh),255); sd=ImageDraw.Draw(sheet)
    for i,(label,im) in enumerate(ims):
        x=(i%cols)*cellw+9; y=(i//cols)*cellh+8
        sheet.paste(im,(x,y)); sd.text((x,y+244),label,font=FONT_SMALL,fill=0)
    sheet.save(ROOT/'tools/tarot-sprite-sheet.png')
    icon.resize((248,256),Image.Resampling.NEAREST).save(ROOT/'tools/tarot-icon-preview.png')
    print('generated',len(CARDS),'cards + back + icon')
    for p in sorted(ART.glob('*.bmp')):
        data=p.read_bytes(); assert data[:2]==b'BM'; assert struct.unpack_from('<H',data,28)[0]==1
        assert struct.unpack_from('<i',data,18)[0]==BMP_W and struct.unpack_from('<i',data,22)[0]==BMP_H
    print('asset validation OK')

if __name__=='__main__': main()
