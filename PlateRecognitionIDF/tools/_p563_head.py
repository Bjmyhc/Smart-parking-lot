# --- 可移植路径自举 (Tool/relocate_paths.py 注入, 勿手改) -------------------
import os as _os

_HERE_ = _os.path.dirname(_os.path.abspath(__file__))


def _find_repo_root_(start):
    """从 start 逐级上溯, 找到同时含 Tool/ 与 Node/ 的目录 = 仓库根。"""
    cur = start
    while True:
        if _os.path.isdir(_os.path.join(cur, 'Tool')) and \
           _os.path.isdir(_os.path.join(cur, 'Node')):
            return cur
        parent = _os.path.dirname(cur)
        if parent == cur:
            return _os.path.dirname(start)
        cur = parent


_REPO_ROOT_ = _find_repo_root_(_HERE_)
_AI_ROOT_ = _os.path.dirname(_REPO_ROOT_)
# --- 自举结束 ---------------------------------------------------------------

# -*- coding: utf-8 -*-
import os, glob
import numpy as np, cv2
D = _AI_ROOT_ + r'\BY串口助手\dist\saved\images'
OUT = _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\p563'
os.makedirs(OUT, exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
fs = sorted(glob.glob(os.path.join(D, "IMG_20261001_2045*.jpg")))
fs = [p for p in fs if load(p).shape[0] == 240]
W = 640
rows = []
info = []
for i, p in enumerate(fs):
    im = load(p)
    b, g, r = im[:,:,0].astype(int), im[:,:,1].astype(int), im[:,:,2].astype(int)
    m = (g > 150) & (r < 120) & (b < 120)
    ys, xs = np.nonzero(m)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    w = x1 - x0 + 1; h = y1 - y0 + 1
    d1 = int(round(w / 7.3))
    head = im[y0:y1+1, x0:x0+d1]
    head_big = cv2.resize(head, (head.shape[1]*10, head.shape[0]*10), interpolation=cv2.INTER_NEAREST)
    if head_big.shape[1] > W: head_big = head_big[:, :W]
    canvas = np.full((head_big.shape[0], W, 3), 255, np.uint8)
    canvas[:, :head_big.shape[1]] = head_big
    bar = np.full((20, W, 3), 255, np.uint8)
    cv2.putText(bar, "FULL %d  green box %dx%d -> head %dx%d, shown x10" % (i, w, h, d1, h), (4, 14),
                cv2.FONT_HERSHEY_SIMPLEX, 0.42, (0,0,0), 1, cv2.LINE_AA)
    rows.append(bar); rows.append(canvas)
    info.append((i, w, h, d1))
cv2.imwrite(os.path.join(OUT, "m_head_from_full.png"), np.vstack(rows))
print(info)
