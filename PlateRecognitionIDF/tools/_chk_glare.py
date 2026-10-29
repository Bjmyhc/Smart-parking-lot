# -*- coding: utf-8 -*-
"""把实拍 94x24 裁块整体调亮/加泛白, 看旧模型 vs r4 谁先崩."""
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
import torch
ROOT = _REPO_ROOT_ + r'\PlateRecognitionIDF'
LPRNET = _AI_ROOT_ + r'\LPRNet_Pytorch'
sys.path.insert(0, os.path.join(ROOT,'tools')); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
CROPS = _AI_ROOT_ + r'\BY串口助手\dist\saved\images'
blank = len(CHARS)-1

def dec(a):
    a = np.squeeze(np.asarray(a, dtype=np.float32))
    if a.shape[0] != len(CHARS): a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank: out.append(c)
        prev = c
    return "".join(CHARS[i] for i in out)

def to_tensor(im):
    im = cv2.resize(im, (94, 24)).astype(np.float32)
    im = (im - 127.5) * 0.0078125
    return im.transpose(2, 0, 1)[None]

torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT,'tools','out','finetune','r4_lr1e4_nofreeze.pth'), map_location='cpu'))
net.eval()
s_old = ort.InferenceSession(os.path.join(ROOT,'tools','out','lprnet_s3.onnx'), providers=['CPUExecutionProvider'])

files = []
for d in sorted(os.listdir(CROPS)):
    p = os.path.join(CROPS, d)
    if os.path.isdir(p):
        files += [(f, d.replace('-','')) for f in glob.glob(os.path.join(p, '*.jpg'))]
files += [(f, '京Q06666') for f in sorted(glob.glob(os.path.join(CROPS, 'IMG_20261001_1740*.jpg')))]
# 只看京Q这块牌
files = [(f, g) for f, g in files if g.startswith("京Q")]
print("样本: %d 张 (京Q06666 实拍裁块)" % len(files))
imgs = [(cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR), g) for f, g in files]

print("%-10s | %-22s | %-22s" % ("调亮", "old 整串/省字", "r4 整串/省字"))
for delta in (0, 20, 40, 60, 80):
    so = sr = po = pr = 0
    for im, g in imgs:
        x = np.clip(im.astype(np.int16) + delta, 0, 255).astype(np.uint8)
        t = to_tensor(x)
        o = s_old.run(None, {s_old.get_inputs()[0].name: t})[0]
        r1 = dec(o[0] if o.ndim == 3 else o)
        r2 = dec(net(torch.from_numpy(t)).numpy()[0])
        so += (r1 == g); po += (len(r1) > 0 and r1[0] == g[0])
        sr += (r2 == g); pr += (len(r2) > 0 and r2[0] == g[0])
    n = len(imgs)
    print("+%-9d | %3d/%3d=%3.0f%% %3d=%3.0f%%   | %3d/%3d=%3.0f%% %3d=%3.0f%%" % (
        delta, so, n, 100*so/n, po, 100*po/n, sr, n, 100*sr/n, pr, 100*pr/n))
