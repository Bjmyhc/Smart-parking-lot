import numpy as np, onnx, sys
from onnx import numpy_helper
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import inspect_espdl as I

def load(espdl):
    Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
    mode, payload = I.split_edl2(espdl)
    model = Mdl.Model.GetRootAs(payload, 0); g = model.Graph()
    out = {}
    for i in range(g.InitializerLength()):
        t = g.Initializer(i); n = t.RawDataLength()
        if not n: continue
        raw = b''.join(bytes(t.RawData(j).BytesAsNumpy()) for j in range(n))
        out[I.decode(t.Name())] = ([t.Dims(j) for j in range(t.DimsLength())],
                                   float(t.Exponents(0)) if t.ExponentsLength() else 0.0, raw)
    return out

def score(A, W):
    """A, W 都是 (O, I, kH, kW) 或 (O,I); 返回逐输出通道相关均值"""
    A = A.reshape(A.shape[0], -1); W = W.reshape(W.shape[0], -1)
    if A.shape != W.shape: return None
    cs = []
    for o in range(A.shape[0]):
        x, y = A[o].astype(np.float64), W[o].astype(np.float64)
        if x.std() > 1e-9 and y.std() > 1e-9:
            cs.append(np.corrcoef(x, y)[0, 1])
    return float(np.mean(cs)) if cs else None

f = {x.name: numpy_helper.to_array(x) for x in onnx.load(sys.argv[1]).graph.initializer}
for espdl in sys.argv[2:]:
    q = load(espdl)
    print('=== %s ===' % espdl)
    for name in ['container.0.weight', 'onnx::Conv_154', 'onnx::Conv_166']:
        if name not in q: continue
        dims, exp, raw = q[name]
        w = np.asarray(f[name]).astype(np.float64)
        O, Ix = w.shape[0], w.shape[1]
        n = int(np.prod(dims))
        a = np.frombuffer(raw, dtype=np.int8)[:n].astype(np.float64)
        res = []
        for tag, num in (('dims 原样 reshape', None), ('dims 反转 reshape', None)):
            pass
        cands = {}
        # 4D 情形: dims = [kH,kW,X,Y]
        kH, kW, X, Y = dims[0], dims[1], dims[2], dims[3]
        cands['[kH,kW,I,O]->OIHW'] = a.reshape(kH, kW, X, Y).transpose(3, 2, 0, 1)
        cands['[kH,kW,O,I]->OIHW'] = a.reshape(kH, kW, Y, X).transpose(2, 3, 0, 1)
        cands['[O,I,kH,kW] 平铺']  = a.reshape(O, Ix, kH, kW)
        cands['[I,O,kH,kW] 平铺']  = a.reshape(Ix, O, kH, kW).transpose(1, 0, 2, 3)
        out = []
        for tag, A in cands.items():
            if A.shape != w.shape: continue
            s = score(A, w)
            if s is not None: out.append((s, tag))
        out.sort(reverse=True)
        print('   %-20s dims=%s -> %s' % (name, dims, ', '.join('%.4f[%s]' % (s, t) for s, t in out)))