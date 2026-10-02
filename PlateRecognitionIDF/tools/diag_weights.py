# -*- coding: utf-8 -*-
"""Compare int8 weights inside an .espdl with the float weights of the ONNX.
If they correlate ~1 the deployed model uses the same weights; if ~0, it does not.
"""
import os, sys
import numpy as np
import onnx
from onnx import numpy_helper

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import inspect_espdl as I

ESPDL = sys.argv[1]
ONNX = sys.argv[2]

Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
mode, payload = I.split_edl2(ESPDL)
model = Mdl.Model.GetRootAs(payload, 0)
g = model.Graph()

q = {}
for i in range(g.InitializerLength()):
    t = g.Initializer(i)
    name = I.decode(t.Name())
    raw = None
    try:
        n = t.RawDataLength()
        if n:
            chunks = [bytes(t.RawData(j).BytesAsNumpy()) for j in range(n)]
            raw = b''.join(chunks)
    except Exception:
        raw = None
    dims = [t.Dims(j) for j in range(t.DimsLength())]
    if raw:
        q[name] = (dims, np.frombuffer(raw, dtype=np.int8))

m = onnx.load(ONNX)
f = {}
for init in m.graph.initializer:
    f[numpy_helper.to_array(init).dtype.kind if False else init.name] = numpy_helper.to_array(init)

print('espdl initializers: %d | onnx initializers: %d' % (len(q), len(f)))
print('%-34s %8s %8s %9s %9s' % ('name', 'n_espdl', 'n_onnx', 'corr', 'rel_err'))
print('-' * 76)
hit = 0
for name in sorted(q):
    if name not in f:
        continue
    dims, arr = q[name]
    w = np.asarray(f[name]).astype(np.float64).ravel()
    if arr is None or arr.size != w.size or arr.size < 16:
        continue
    a = arr.astype(np.float64)
    if a.std() < 1e-9 or w.std() < 1e-9:
        continue
    corr = float(np.corrcoef(a, w)[0, 1])
    wq = np.round(w / (np.abs(w).max() / 127.0))
    rel = float(np.abs(a - wq).mean() / max(1e-9, np.abs(wq).mean()))
    print('%-34s %8d %8d %9.4f %9.4f' % (name[:34], a.size, w.size, corr, rel))
    hit += 1
print('对比了 %d 个共同名字的权重' % hit)