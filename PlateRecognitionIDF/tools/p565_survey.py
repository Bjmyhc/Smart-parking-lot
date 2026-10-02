# -*- coding: utf-8 -*-
import os, glob, collections
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
roots = [
 r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out",
 r"G:\All_Project\AI_Project\Smart-Paring-Iot\Doc\硬件资料\车牌测试样例",
 r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\esp32cam",
]
for r in roots:
    if not os.path.isdir(r): print("MISSING", r); continue
    print("=== ", r)
    for d in sorted(os.listdir(r)):
        sub = os.path.join(r, d)
        if not os.path.isdir(sub): continue
        fs = [f for f in os.listdir(sub) if f.lower().endswith((".jpg",".jpeg",".png"))]
        if not fs: continue
        im = load(os.path.join(sub, fs[0]))
        print("  %-16s n=%4d  例子尺寸 %s" % (d, len(fs), (im.shape if im is not None else None)))
    fs = [f for f in os.listdir(r) if f.lower().endswith((".jpg",".jpeg",".png"))]
    if fs:
        im = load(os.path.join(r, fs[0]))
        print("  (根目录) n=%d 例子 %s %s" % (len(fs), fs[0], im.shape if im is not None else None))
