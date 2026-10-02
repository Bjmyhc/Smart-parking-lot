# -*- coding: utf-8 -*-
import glob, os
import numpy as np, cv2
d = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
out = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p562"
os.makedirs(out, exist_ok=True)
fs = sorted(glob.glob(os.path.join(d, "IMG_20261001_2045*.jpg")))
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
big = [p for p in fs if load(p).shape[0] == 240]
sml = [p for p in fs if load(p).shape[0] == 24]
rows = []
for i, p in enumerate(big):
    im = load(p)
    im = cv2.resize(im, (im.shape[1]*2, im.shape[0]*2), interpolation=cv2.INTER_NEAREST)
    bar = np.full((22, im.shape[1], 3), 255, np.uint8)
    cv2.putText(bar, "FULL %d  %s" % (i, os.path.basename(p)[-10:-4]), (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,0,0), 1, cv2.LINE_AA)
    rows.append(bar); rows.append(im)
cv2.imwrite(os.path.join(out, "m_full.png"), np.vstack(rows))
rows = []
for i, p in enumerate(sml):
    im = load(p)
    im = cv2.resize(im, (im.shape[1]*6, im.shape[0]*6), interpolation=cv2.INTER_NEAREST)
    bar = np.full((22, im.shape[1], 3), 255, np.uint8)
    cv2.putText(bar, "CROP %d  %s" % (i, os.path.basename(p)[-10:-4]), (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0,0,0), 1, cv2.LINE_AA)
    rows.append(bar); rows.append(im)
cv2.imwrite(os.path.join(out, "m_crop.png"), np.vstack(rows))
print("full:", len(big), "crop:", len(sml))
for p in big: print("  ", os.path.basename(p))
for p in sml: print("  ", os.path.basename(p))
