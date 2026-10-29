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

import os, glob, collections
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for d in [_REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\collect2\京Q06666',
          _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\collect2\粤T666FP']:
    fs = glob.glob(os.path.join(d, "*"))
    if fs:
        im = load(fs[0])
        print(os.path.basename(d), len(fs), "first:", os.path.basename(fs[0]), im.shape if im is not None else None)
for sub in ["train", "val"]:
    d = os.path.join(_AI_ROOT_ + r'\LPRNet_Pytorch\data\ccpd_plates', sub)
    c = collections.Counter()
    n = 0
    for f in os.listdir(d):
        c[f[0]] += 1; n += 1
        if n > 9000: break
    print("ccpd_plates/%-6s n=%d  provinces=%d  top: %s" % (sub, n, len(c), " ".join("%s%d"%(k,v) for k,v in c.most_common(6))))
