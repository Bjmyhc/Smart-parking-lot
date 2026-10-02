import numpy as np, onnx, sys
from onnx import numpy_helper
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import inspect_espdl as I
Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
mode, payload = I.split_edl2(sys.argv[1])
model = Mdl.Model.GetRootAs(payload, 0); g = model.Graph()
q = {}
for i in range(g.InitializerLength()):
    t = g.Initializer(i); n = t.RawDataLength()
    if not n: continue
    raw = b''.join(bytes(t.RawData(j).BytesAsNumpy()) for j in range(n))
    q[I.decode(t.Name())] = ([t.Dims(j) for j in range(t.DimsLength())],
                             float(t.Exponents(0)) if t.ExponentsLength() else 0.0, raw)
f = {x.name: numpy_helper.to_array(x) for x in onnx.load(sys.argv[2]).graph.initializer}

def rep(name, A, B):
    A = A.ravel().astype(np.float64); B = B.ravel().astype(np.float64)
    c = np.corrcoef(A, B)[0, 1]
    # per-row (output channel) correlation
    return c

for name in ['container.0.weight', 'onnx::Conv_160', 'backbone.4.block.0.weight', 'onnx::Conv_154']:
    dims, exp, raw = q[name]
    w = np.asarray(f[name]).astype(np.float64)
    O = w.shape[0]; Ix = w.shape[1]
    kH, kW = dims[0], dims[1]
    nI, nO = dims[2], dims[3]
    a = np.frombuffer(raw, dtype=np.int8)
    print('%s dims=%s onnx=%s' % (name, dims, w.shape))
    # candidate 1: [kH,kW,I,O]
    c1 = a[:kH*kW*nI*nO].reshape(kH, kW, nI, nO).transpose(3, 2, 0, 1)
    # candidate 2: [kH,kW,O,I]
    c2 = a[:kH*kW*nI*nO].reshape(kH, kW, nO, nI).transpose(2, 3, 0, 1)
    for tag, A in (('kHkWIO', c1), ('kHkW OI', c2)):
        if A.shape != w.shape: 
            print('   %s shape %s 不匹配' % (tag, A.shape)); continue
        # per output channel correlation
        cs = []
        for o in range(min(O, 8)):
            x = A[o].ravel().astype(np.float64); y = w[o].ravel().astype(np.float64)
            if x.std() > 1e-9 and y.std() > 1e-9:
                cs.append(np.corrcoef(x, y)[0, 1])
        print('   %-9s 全局相关 %7.4f | 逐输出通道相关 %.4f' % (tag, rep(name, A, w), float(np.mean(cs)) if cs else float('nan')))