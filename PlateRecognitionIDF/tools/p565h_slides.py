# -*- coding: utf-8 -*-
"""p565h: 生成"每省一张"的拍摄用幻灯片(全屏展示用), 素材取本地 CCPD 真实车牌."""
import os, glob, random
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565", "capture_slides"); os.makedirs(OUT, exist_ok=True)
CCPD = r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\ccpd_plates\train"
PROV = list("京津冀晋蒙辽吉黑沪苏浙皖闽赣鲁鄂湘粤桂琼渝川贵云藏陕甘青宁新")  # 豫 单独从本地真实素材取
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
pool = {}
for f in os.listdir(CCPD):
    c = f[0]
    pool.setdefault(c, []).append(f)
rng = random.Random(0)
made = []
for ch in PROV:
    fs = pool.get(ch) or []
    if not fs: print("缺:", ch); continue
    p = os.path.join(CCPD, rng.choice(fs))
    im = load(p)
    if im is None: continue
    H, W = 900, 1600
    canvas = np.full((H, W, 3), 240, np.uint8)
    s = min((W*0.8)/im.shape[1], (H*0.55)/im.shape[0])
    r = cv2.resize(im, (int(im.shape[1]*s), int(im.shape[0]*s)), interpolation=cv2.INTER_CUBIC)
    y0 = (H-r.shape[0])//2; x0 = (W-r.shape[1])//2
    canvas[y0:y0+r.shape[0], x0:x0+r.shape[1]] = r
    cv2.imwrite(os.path.join(OUT, "%s.png" % ch), canvas)
    made.append(ch)
print("生成 %d 张: %s" % (len(made), "".join(made)))
