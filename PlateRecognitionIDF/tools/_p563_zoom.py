# -*- coding: utf-8 -*-
import os, glob
import numpy as np, cv2
D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p563"
os.makedirs(OUT, exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
fs = sorted([p for p in glob.glob(os.path.join(D, "IMG_20261001_2045*.jpg")) if load(p).shape[0] == 24])
rows = []
for i, p in enumerate(fs):
    im = load(p)
    head = im[:, 0:13]                      # 省字那一格
    big = cv2.resize(head, (13*14, 24*14), interpolation=cv2.INTER_NEAREST)
    bar = np.full((20, big.shape[1], 3), 255, np.uint8)
    cv2.putText(bar, "CROP %d first char" % i, (6, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0,0,0), 1, cv2.LINE_AA)
    rows.append(bar); rows.append(big)
cv2.imwrite(os.path.join(OUT, "m_firstchar.png"), np.vstack(rows))
print("ok", len(fs))
