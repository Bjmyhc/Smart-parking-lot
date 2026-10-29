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

import sys, numpy as np, torch
sys.path.insert(0, _AI_ROOT_ + r'\LPRNet_Pytorch')
sys.path.insert(0, _REPO_ROOT_ + r'\PlateRecognitionIDF\tools')
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.eval()
torch.set_grad_enabled(False)
y = net(torch.zeros(1, 3, 24, 94))
print("CHARS", len(CHARS), "out shape", tuple(y.shape))
