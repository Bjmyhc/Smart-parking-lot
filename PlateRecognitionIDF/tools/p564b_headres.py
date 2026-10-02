# -*- coding: utf-8 -*-
"""p564b: 同一个省字两种分辨率并排 —— 模型输入里的(13x22) vs 原图里的(23x79, 固件里是46x158)."""
import os, glob
import numpy as np, cv2
D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p564"
os.makedirs(OUT, exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def bar(t, w, h=26, fs=0.55):
    b = np.full((h, w, 3), 255, np.uint8); cv2.putText(b, t, (8, 18), cv2.FONT_HERSHEY_SIMPLEX, fs, (0,0,0), 1, cv2.LINE_AA); return b
crop = load(os.path.join(D, "IMG_20261001_204553_610967.jpg"))     # 94x24 模型输入
full = load(os.path.join(D, "IMG_20261001_204548_710388.jpg"))     # 320x240 原图
b, g, r = full[:,:,0].astype(int), full[:,:,1].astype(int), full[:,:,2].astype(int)
m = (g > 150) & (r < 120) & (b < 120)
ys, xs = np.nonzero(m)
x0, x1, y0, y1 = xs.min()+6, xs.max()-6, ys.min()+5, ys.max()-5     # 内缩, 去掉绿线
w = x1-x0+1; h = y1-y0+1
head_src = full[y0:y1+1, x0:x0+int(round(w/7.3))]
head_model = crop[1:23, 0:13]
def up(img, target_h):
    s = target_h / img.shape[0]
    return cv2.resize(img, (int(img.shape[1]*s), target_h), interpolation=cv2.INTER_NEAREST)
TH = 400
a = up(head_model, TH)      # 模型输入里的省字
bb = up(head_src, TH)       # 原图里的省字
gap = np.full((TH, 24, 3), 255, np.uint8)
pair = np.hstack([a, gap, bb])
W = pair.shape[1] + 20
canvas = np.full((pair.shape[0], W, 3), 250, np.uint8); canvas[:, :pair.shape[1]] = pair
rows = [bar("左: 模型输入里的省字 %dx%d (放大约  %dx)" % (head_model.shape[1], head_model.shape[0], int(TH/head_model.shape[0])), W),
        bar("右: 原图里同一个省字 %dx%d (固件 640x480 里是 %dx%d)" % (head_src.shape[1], head_src.shape[0], head_src.shape[1]*2, head_src.shape[0]*2), W),
        canvas]
cv2.imwrite(os.path.join(OUT, "head_resolution.png"), np.vstack(rows))
print("模型输入里的省字: %dx%d" % (head_model.shape[1], head_model.shape[0]))
print("原图(320x240)里的省字: %dx%d  -> 固件 640x480 里: %dx%d" % (head_src.shape[1], head_src.shape[0], head_src.shape[1]*2, head_src.shape[0]*2))
print("像素数比: 模型 %.0f 个  vs  原图(320x240) %.0f 个  vs  固件原图 %.0f 个" %
      (head_model.shape[0]*head_model.shape[1], head_src.shape[0]*head_src.shape[1], head_src.shape[0]*head_src.shape[1]*4))
