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
import sys, numpy as np, onnxruntime as ort
sys.path.insert(0, _AI_ROOT_ + r'\LPRNet_Pytorch')
from data.load_data import CHARS
ROOT = _REPO_ROOT_ + r'\PlateRecognitionIDF'
BIN  = ROOT + r"\main\models\test_input.bin"
blank = len(CHARS) - 1

def run(onnx, tag):
    x = np.fromfile(BIN, dtype="<f4").reshape(24, 94, 3).transpose(2, 0, 1)[None]
    s = ort.InferenceSession(onnx, providers=["CPUExecutionProvider"])
    o = s.run(None, {s.get_inputs()[0].name: x})[0]
    p = o[0] if o.ndim == 3 else o
    lab = [int(np.argmax(p[:, j])) for j in range(p.shape[1])]
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank: out.append(c)
        prev = c
    print("%-34s -> %s" % (tag, "".join(CHARS[i] for i in out)))

run(ROOT + r"\tools\out\lprnet_s3.onnx", "lprnet_s3.onnx (tools/out)")
run(ROOT + r"\tools\out\r4_s3.onnx",      "r4_s3.onnx")
