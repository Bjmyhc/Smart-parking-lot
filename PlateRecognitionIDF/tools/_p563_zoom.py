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
