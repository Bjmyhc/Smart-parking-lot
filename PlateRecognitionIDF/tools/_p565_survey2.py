import os, glob
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for sub in ["collect2","collect","hard","train2","holdout","row"]:
    d = os.path.join(r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out", sub)
    if not os.path.isdir(d): continue
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if os.path.isdir(p):
            fs = [f for f in os.listdir(p) if f.lower().endswith((".jpg",".png",".jpeg"))]
            im = load(os.path.join(p, fs[0])) if fs else None
            print("%-10s / %-14s n=%4d  例 %s" % (sub, name, len(fs), im.shape if im is not None else None))
        else:
            if name.lower().endswith((".jpg",".png",".jpeg")):
                im = load(p); print("%-10s / %-14s     例 %s" % (sub, name[:14], im.shape if im is not None else None))
d = r"G:\All_Project\AI_Project\Smart-Paring-Iot\Doc\硬件资料\车牌测试样例"
for f in sorted(os.listdir(d)):
    im = load(os.path.join(d, f)); print("样例 / %-20s %s" % (f, im.shape if im is not None else None))
