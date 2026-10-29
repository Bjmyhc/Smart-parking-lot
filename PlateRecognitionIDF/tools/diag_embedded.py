# -*- coding: utf-8 -*-
"""直接吃板端内嵌样张 test_input.bin (float32 HWC), 看 PC 端模型读成什么。"""
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

import sys, numpy as np, onnxruntime as ort
sys.path.insert(0, _AI_ROOT_ + r'\LPRNet_Pytorch')
from data.load_data import CHARS

ROOT = _REPO_ROOT_ + r'\PlateRecognitionIDF'
ONNX = ROOT + r'\tools\out\lprnet_s3.onnx'
BIN  = ROOT + r'\main\models\test_input.bin'

x = np.fromfile(BIN, dtype='<f4').reshape(24, 94, 3)
x = x.transpose(2, 0, 1)[None]
sess = ort.InferenceSession(ONNX, providers=['CPUExecutionProvider'])
inp = sess.get_inputs()[0]
o = sess.run(None, {inp.name: x})[0]
p = o[0] if o.ndim == 3 else o
blank = len(CHARS) - 1
lab = [int(np.argmax(p[:, j])) for j in range(p.shape[1])]
out, prev = [], blank
for c in lab:
    if c != prev and c != blank: out.append(c)
    prev = c
print('PC(float) 读 test_input.bin (真值标签 沪AMS087) -> %s' % ''.join(CHARS[i] for i in out))
tops = []
for j, c in enumerate(lab):
    t = np.argsort(p[:, j])[::-1][:2]
    tops.append('%s%.0f(%s%.0f)' % (CHARS[t[0]], 100*p[t[0], j], CHARS[t[1]], 100*p[t[1], j]))
print('每步 top1(次选): ' + ' '.join(tops))