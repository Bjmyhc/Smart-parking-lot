# -*- coding: utf-8 -*-
"""Compare espdl int8 weights vs ONNX float weights with the correct layout mapping.
espdl conv weight dims are [kH, kW, I, O] (NHWC-style); ONNX is [O, I, kH, kW].
"""
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

import os, sys
import numpy as np
import onnx
from onnx import numpy_helper

sys.path.insert(0, _REPO_ROOT_ + r'\PlateRecognitionIDF\tools')
import inspect_espdl as I

ESPDL, ONNX = sys.argv[1], sys.argv[2]
Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
mode, payload = I.split_edl2(ESPDL)
model = Mdl.Model.GetRootAs(payload, 0)
g = model.Graph()

q = {}
for i in range(g.InitializerLength()):
    t = g.Initializer(i)
    n = t.RawDataLength()
    if not n:
        continue
    raw = b''.join(bytes(t.RawData(j).BytesAsNumpy()) for j in range(n))
    q[I.decode(t.Name())] = ([t.Dims(j) for j in range(t.DimsLength())],
                             float(t.Exponents(0)) if t.ExponentsLength() else 0.0, raw)

f = {init.name: numpy_helper.to_array(init) for init in onnx.load(ONNX).graph.initializer}

print('%-32s %-16s %-16s %8s' % ('name', 'espdl dims', 'onnx shape', 'corr'))
print('-' * 80)
cors = []
for name in sorted(q):
    if name not in f:
        continue
    dims, exp, raw = q[name]
    w = np.asarray(f[name]).astype(np.float64)
    if len(dims) == 4:
        kH, kW, nI, nO = dims
        if w.shape != (nO, nI, kH, kW):
            continue
        a = np.frombuffer(raw, dtype=np.int8)[:nI * nO * kH * kW].reshape(nI, nO, kH, kW)
        a = np.transpose(a, (1, 0, 2, 3)).astype(np.float64)          # -> O,I,kH,kW
    elif len(dims) == 1:
        if w.shape != (dims[0],):
            continue
        a = np.frombuffer(raw, dtype=np.int32 if w.dtype.kind == 'f' and False else np.int32)[:dims[0]].astype(np.float64)
        a = np.frombuffer(raw, dtype=np.int32)[:dims[0]].astype(np.float64)
    else:
        continue
    aa, ww = a.ravel(), w.ravel()
    if aa.size != ww.size or aa.std() < 1e-9 or ww.std() < 1e-9:
        continue
    c = float(np.corrcoef(aa, ww)[0, 1])
    cors.append(c)
    print('%-32s %-16s %-16s %8.4f' % (name[:32], str(dims)[:16], str(tuple(w.shape))[:16], c))
print('')
print('平均相关: %.4f   (≈1 => 同一份权重; ≈0 => 完全不同的模型)' % (float(np.mean(cors)) if cors else 0.0))