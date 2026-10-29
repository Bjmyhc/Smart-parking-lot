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

import os, glob
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for sub in ["collect2","collect","hard","train2","holdout","row"]:
    d = os.path.join(_REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out', sub)
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
d = _REPO_ROOT_ + r'\Doc\硬件资料\车牌测试样例'
for f in sorted(os.listdir(d)):
    im = load(os.path.join(d, f)); print("样例 / %-20s %s" % (f, im.shape if im is not None else None))
