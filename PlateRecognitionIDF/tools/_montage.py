# -*- coding: utf-8 -*-
import os, glob
from PIL import Image, ImageDraw, ImageFont
D = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
OUT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\board_crops_with_output.png'
files = sorted(glob.glob(os.path.join(D, 'IMG_20261001_1740*.jpg')))
labels = [('#96', '京Q粤'), ('#97', '京粤Q粤浙粤'), ('#98', '粤京粤'), ('#99', '京Q粤'),
          ('#100', '京粤'), ('#101', '京粤粤粤'), ('#102', '京Q粤粤'), ('#103', '浙京Q粤粤粤'),
          ('#104', '粤京粤粤'), ('#105', '粤浙粤浙粤浙粤浙粤')]
S, LAB = 6, 26
cw, ch = 94*S, 24*S
cols, rows = 2, (len(files)+1)//2
im = Image.new('RGB', (cw*cols, (ch+LAB)*rows), (18,18,18))
dr = ImageDraw.Draw(im)
try:
    font = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 18)
except Exception:
    font = ImageFont.load_default()
for i, f in enumerate(files):
    c, r = i % cols, i // cols
    x, y = c*cw, r*(ch+LAB)
    img = Image.open(f).convert('RGB').resize((cw, ch), Image.NEAREST)
    im.paste(img, (x, y+LAB))
    tag, out = labels[i]
    dr.text((x+6, y+4), u'%s  \u677f\u7aef\u8bfb\u6210: %s' % (tag, out), fill=(255,220,0), font=font)
im.save(OUT)
print('saved', OUT, im.size)