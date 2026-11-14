#!/usr/bin/env python3
"""Draws the 128x128 NORTH POLE SANTA COMMUNICATOR screen.

    pip install pillow
    python3 design/make_screen.py [path/to/MountainsofChristmas-Bold.ttf]

Font: Mountains of Christmas (Google Fonts, SIL Open Font License 1.1),
https://fonts.google.com/specimen/Mountains+of+Christmas . Pass its path, set
NPSC_FONT, or drop the .ttf next to this script. Writes
esp32/components/muse/avatar/npsc_art.png and design/previews/screen-512.png;
then run make_art.py in that avatar folder to refresh npsc_art.h.
"""
import os, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
HERE=Path(__file__).resolve().parent
MC=sys.argv[1] if len(sys.argv)>1 else os.environ.get("NPSC_FONT", str(HERE/"MountainsofChristmas-Bold.ttf"))
RED=(179,18,31); GOLD=(255,213,74); WHITE=(255,255,255); GREEN=(30,140,60); BROWN=(110,60,20); PINK=(230,40,50)
pal={"w":WHITE,"y":GOLD,"g":GREEN,"o":(240,140,30),"r":PINK,"b":BROWN}
def sprite(d,x,y,rows,p,k=1):
    for j,r in enumerate(rows):
        for i,c in enumerate(r):
            if c in p: d.rectangle((x+i*k,y+j*k,x+i*k+k-1,y+j*k+k-1),fill=p[c])
FLAKE=["..w.w..",".w.w.w.","w.www.w",".wwwww.","w.www.w",".w.w.w.","..w.w.."]
TREE=["....y....","...ygy...","....g....","...ggg...","..gogg...","..ggggg..",".ggggrgg.","..ggggg..",".ggrgggg.","ggggggogg","....b....","....b...."]
GIFT=["..y.y..","...y...","rrryrrr","rrryrrr","yyyyyyy","rrryrrr","rrryrrr","rrryrrr"]
HOLLY=["gg.....gg","ggg...ggg",".gggrggg.","..grrrg..","....r...."]
def ctext(d,y,t,f,c):
    w=d.textlength(t,font=f); d.text(((128-w)/2,y),t,font=f,fill=c)
im=Image.new("RGB",(128,128),RED); d=ImageDraw.Draw(im)
d.rectangle((2,2,125,125),outline=WHITE,width=2)
sprite(d,7,7,FLAKE,pal); sprite(d,114,7,FLAKE,pal); sprite(d,55,5,HOLLY,pal,2)
ctext(d,15,"NORTH POLE",ImageFont.truetype(MC,15),WHITE)
ctext(d,29,"SANTA",ImageFont.truetype(MC,30),GOLD)
ctext(d,62,"COMMUNICATOR",ImageFont.truetype(MC,12),GOLD)
sprite(d,55,82,TREE,pal,2); sprite(d,34,90,GIFT,pal,2); sprite(d,80,90,GIFT,{**pal,'r':GREEN},2)
for x,y in [(12,52),(114,58),(10,32),(116,38),(22,88),(106,86)]: d.point((x,y),fill=WHITE)
d.line((5,107,122,107),fill=WHITE)
lf=ImageFont.truetype(MC,11)
w=d.textlength("MENU",font=lf); d.text((22-w/2,108),"MENU",font=lf,fill=WHITE)
t="COMMUNICATE"; w=d.textlength(t,font=lf); d.text((121-w,108),t,font=lf,fill=WHITE)
for cx in (22,108): d.polygon([(cx-3,120),(cx+3,120),(cx,123)],fill=GOLD)
im.save(HERE.parent/"esp32/components/muse/avatar/npsc_art.png")
im.resize((512,512),Image.NEAREST).save(HERE/"previews/screen-512.png")
