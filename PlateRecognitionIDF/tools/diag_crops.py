# -*- coding: utf-8 -*-
"""把板端发回来的 94x24 模型输入块直接喂给 PC 端 ONNX, 对比板端输出。"""
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

import os, sys, glob
import numpy as np, cv2, onnxruntime as ort
sys.path.insert(0, _AI_ROOT_ + r'\LPRNet_Pytorch')
from data.load_data import CHARS

ONNX = sys.argv[1] if len(sys.argv) > 1 else _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\lprnet_s3.onnx'
DIR  = sys.argv[2] if len(sys.argv) > 2 else _AI_ROOT_ + r'\BY串口助手\dist\saved\images'
PATS = sys.argv[3] if len(sys.argv) > 3 else 'IMG_20261001_1740*.jpg'

CH = CHARS
BLANK = len(CH) - 1
sess = ort.InferenceSession(ONNX, providers=['CPUExecutionProvider'])
inp = sess.get_inputs()[0]
print('onnx=%s' % ONNX)
print('input %s %s | CHARS n=%d blank=%d' % (inp.name, inp.shape, len(CH), BLANK))

def decode(p):
    lab = [int(np.argmax(p[:, j])) for j in range(p.shape[1])]
    out, prev = [], BLANK
    for c in lab:
        if c != prev and c != BLANK:
            out.append(c)
        prev = c
    return ''.join(CH[i] for i in out), lab

files = sorted(glob.glob(os.path.join(DIR, PATS)))
print('files=%d' % len(files))
for f in files:
    img = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        print('skip %s' % f); continue
    x = cv2.resize(img, (94, 24)).astype(np.float32)
    x = (x - 127.5) * 0.0078125
    x = x.transpose(2, 0, 1)[None]
    o = sess.run(None, {inp.name: x})[0]
    p = o[0] if o.ndim == 3 else o
    txt, lab = decode(p)
    steps = []
    for j, c in enumerate(lab):
        top = np.argsort(p[:, j])[::-1][:2]
        steps.append('%s%.0f(%s%.0f)' % (CH[top[0]], 100*p[top[0], j], CH[top[1]], 100*p[top[1], j]))
    print('%-32s -> %-14s | %s' % (os.path.basename(f), txt, ' '.join(steps)))