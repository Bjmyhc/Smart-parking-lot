# -*- coding: utf-8 -*-
"""p564: 演示"人眼看到的 vs 模型拿到的" —— 说明为什么肉眼清楚、模型看不清."""
import os, glob
import numpy as np, cv2
D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p564"
os.makedirs(OUT, exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def bar(text, w, h=26, fs=0.6, color=(0,0,0)):
    b = np.full((h, w, 3), 255, np.uint8)
    cv2.putText(b, text, (8, int(h*0.72)), cv2.FONT_HERSHEY_SIMPLEX, fs, color, 1, cv2.LINE_AA)
    return b
full = load(os.path.join(D, "IMG_20261001_204548_710388.jpg"))     # 全图(带绿框)
crop = load(os.path.join(D, "IMG_20261001_204553_610967.jpg"))     # 模型输入 94x24
# 1) 全图里车牌附近
b, g, r = full[:,:,0].astype(int), full[:,:,1].astype(int), full[:,:,2].astype(int)
m = (g > 150) & (r < 120) & (b < 120)
ys, xs = np.nonzero(m)
pad = 18
y0, y1 = max(0, ys.min()-pad), min(full.shape[0], ys.max()+pad)
x0, x1 = max(0, xs.min()-pad), min(full.shape[1], xs.max()+pad)
roi = full[y0:y1, x0:x1]
Z = 4
rowA = cv2.resize(roi, (roi.shape[1]*Z, roi.shape[0]*Z), interpolation=cv2.INTER_NEAREST)
# 2) 94x24 模型输入, 放大 10 倍, 红框标省字
Z2 = 10
big = cv2.resize(crop, (crop.shape[1]*Z2, crop.shape[0]*Z2), interpolation=cv2.INTER_NEAREST)
cv2.rectangle(big, (0, 0), (13*Z2-1, 24*Z2-1), (0, 0, 255), 3)
# 3) 省字单独
head = crop[1:23, 0:13]
Z3 = 26
hb = cv2.resize(head, (head.shape[1]*Z3, head.shape[0]*Z3), interpolation=cv2.INTER_NEAREST)
W = max(rowA.shape[1], big.shape[1], hb.shape[1], 900)
def canvas(img):
    c = np.full((img.shape[0], W, 3), 250, np.uint8); c[:, :img.shape[1]] = img; return c
rows = [bar("(1) 你眼睛看到的: 原图 320x240 里的车牌 (固件里其实是 640x480, 再大一倍)", W, 28, 0.6),
        canvas(rowA),
        bar("(2) 模型实际拿到的: 整块牌被压成 94x24 (红框=省字那一格, 只有 13 像素宽)", W, 28, 0.6),
        canvas(big),
        bar("(3) 省字单独放大: 13 x 22 像素  =  约 290 个像素, 要装下整个'京'字的 5 个笔画", W, 28, 0.6),
        canvas(hb)]
cv2.imwrite(os.path.join(OUT, "why_unclear.png"), np.vstack(rows))
# 数字
print("原图(320x240)里车牌框: %dx%d px" % (xs.max()-xs.min()+1, ys.max()-ys.min()+1))
print("固件 640x480 里对应:   %dx%d px" % ((xs.max()-xs.min()+1)*2, (ys.max()-ys.min()+1)*2))
print("省字在原图(320x240):   %dx%d px" % ((xs.max()-xs.min()+1)/7.3, ys.max()-ys.min()+1))
print("省字在固件 640x480:    %dx%d px" % ((xs.max()-xs.min()+1)*2/7.3, (ys.max()-ys.min()+1)*2))
print("省字在模型输入 94x24:  %.1f x %d px  <-- 模型只看得到这么多" % (94/7.3, 24))
gray = cv2.cvtColor(head, cv2.COLOR_BGR2GRAY)
print("省字那格的灰度范围: min=%d max=%d 跨度=%d  (反差越小越难认)" % (gray.min(), gray.max(), int(gray.max())-int(gray.min())))
