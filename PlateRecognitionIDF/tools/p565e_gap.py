# -*- coding: utf-8 -*-
"""p565e: 合成图 vs 真实图 摆一起 + 白字外接框统计, 找出 domain gap."""
import os, sys, glob, random
import numpy as np, cv2
sys.path.insert(0, r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools")
import importlib.util
spec = importlib.util.spec_from_file_location("t", r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\p565d_train.py")
# 不执行训练; 直接把 render/augment 抄过来
from PIL import Image, ImageDraw, ImageFont
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
FONT = r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\NotoSansCJK-Regular.ttc"
W, H, SS = 32, 64, 4
def render(ch):
    big = np.zeros((H*SS, W*SS, 3), np.float32)
    gy = np.linspace(-1, 1, H*SS)[:, None]
    big[:,:,0] = 205.0 + gy*18; big[:,:,1] = 150.0 + gy*16; big[:,:,2] = 72.0 + gy*12
    m = Image.new("L", (W*SS, H*SS), 0); d = ImageDraw.Draw(m)
    f = ImageFont.truetype(FONT, int(H*SS*0.80))
    bb = d.textbbox((0,0), ch, font=f)
    d.text(((W*SS-(bb[2]-bb[0]))/2-bb[0], (H*SS-(bb[3]-bb[1]))/2-bb[1]), ch, fill=255, font=f)
    mk = np.asarray(m, np.float32)[:,:,None]/255.0
    img = np.clip(big*(1-mk) + 250.0*mk, 0, 255).astype(np.uint8)
    return cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)
def bbox_white(im):
    # 白字 = 亮度最高的一撮; 用 Otsu 找
    g = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
    t, b = cv2.threshold(g, 0, 255, cv2.THRESH_BINARY+cv2.THRESH_OTSU)
    ys, xs = np.nonzero(b > 0)
    if len(xs) == 0: return None
    return (xs.min(), xs.max(), ys.min(), ys.max(), (b>0).mean())
rows = []; rs = []; ss = []
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for ch, lab in [("京","jing")]:
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png")))[:8]:
        im = load(p); bb = bbox_white(im)
        if bb: rs.append((bb[1]-bb[0]+1, bb[3]-bb[2]+1, bb[4]))
        rows.append(cv2.resize(im, (W*5, H*5), interpolation=cv2.INTER_NEAREST))
    rows.append(np.full((6, W*5, 3), 0, np.uint8))
    for _ in range(8):
        im = render(ch); bb = bbox_white(im)
        if bb: ss.append((bb[1]-bb[0]+1, bb[3]-bb[2]+1, bb[4]))
        rows.append(cv2.resize(im, (W*5, H*5), interpolation=cv2.INTER_NEAREST))
cv2.imwrite(os.path.join(OUT, "real_vs_synth.png"), np.vstack(rows))
def stat(name, v):
    a = np.array(v, float)
    print("%s n=%d  白字宽 %.1f +-%.1f  高 %.1f +-%.1f  占比 %.2f" %
          (name, len(a), a[:,0].mean(), a[:,0].std(), a[:,1].mean(), a[:,1].std(), a[:,2].mean()))
stat("真实", rs); stat("合成", ss)
