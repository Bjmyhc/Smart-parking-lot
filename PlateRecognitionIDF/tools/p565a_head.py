# -*- coding: utf-8 -*-
"""p565a: 从真实车牌图里抠省字, 存成 32x64 的 patch, 并拼图确认裁切规则."""
import os, glob
import numpy as np, cv2
OUT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p565"
os.makedirs(os.path.join(OUT, "real"), exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def head_of(plate):
    h, w = plate.shape[:2]
    x0 = int(round(w * 0.030)); x1 = int(round(w * 0.160))
    y0 = int(round(h * 0.10));  y1 = int(round(h * 0.90))
    return plate[y0:y1, x0:x1]
srcs = []
c2 = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\collect2"
for lab, sub in [("京", "京Q06666"), ("粤", "粤T666FP")]:
    for p in sorted(glob.glob(os.path.join(c2, sub, "*.jpg")))[:12]:
        srcs.append((lab, p))
d = r"G:\All_Project\AI_Project\Smart-Paring-Iot\Doc\硬件资料\车牌测试样例"
for lab, f in [("京", "京Q06666.png"), ("粤", "粤T666FP.jpg"), ("豫", "豫A8F8Q8.jpg"), ("豫", "豫FSQ8I8.jpg")]:
    srcs.append((lab, os.path.join(d, f)))
rows = []
n_saved = 0
for lab, p in srcs:
    im = load(p)
    if im is None: continue
    head = head_of(im)
    big = cv2.resize(head, (32*4, 64*4), interpolation=cv2.INTER_AREA if head.shape[0] > 64 else cv2.INTER_CUBIC)
    bar = np.full((20, big.shape[1]+90, 3), 255, np.uint8)
    import unicodedata
    cv2.putText(bar, "?", (6, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,0,0), 1)
    body = np.full((big.shape[0], big.shape[1]+90, 3), 250, np.uint8)
    body[:, :big.shape[1]] = big
    nm = os.path.basename(p).encode("ascii", "replace").decode()
    cv2.putText(body, nm[:16], (big.shape[1]+4, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0,0,0), 1)
    rows.append(np.vstack([bar, body]))
    n_saved += 1
    if n_saved <= 12:      # 前 12 张(collect2) 存盘
        cv2.imwrite(os.path.join(OUT, "real", "%s_%03d.png" % ("jing" if lab=="京" else "yue", n_saved)), cv2.resize(head, (32,64), interpolation=cv2.INTER_AREA))
cv2.imwrite(os.path.join(OUT, "head_real_check.png"), np.vstack(rows))
print("saved", n_saved)
