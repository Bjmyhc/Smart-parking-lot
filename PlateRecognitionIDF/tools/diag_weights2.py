# -*- coding: utf-8 -*-
"""Compare espdl int8 weights vs ONNX float weights with the correct layout mapping.
espdl conv weight dims are [kH, kW, I, O] (NHWC-style); ONNX is [O, I, kH, kW].
"""
import os, sys
import numpy as np
import onnx
from onnx import numpy_helper

sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
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