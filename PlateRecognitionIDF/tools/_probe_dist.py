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
for name in ['container.0.weight', 'backbone.4.block.0.weight', 'onnx::Conv_154']:
    dims, exp, raw = q[name]
    a = np.frombuffer(raw, dtype=np.int8).astype(np.float64)
    w = np.asarray(f[name]).astype(np.float64).ravel()
    print('%s  espdl dims=%s exp=%g | n=%d' % (name, dims, exp, a.size))
    for tag, arr in (('espdl', a), ('onnx ', w)):
        s = np.sort(arr)
        qs = np.percentile(s, [0, 5, 25, 50, 75, 95, 100])
        print('   %s  min/5/25/50/75/95/max = %s   std=%.4g' % (tag, np.round(qs, 3), s.std()))
    print('   espdl 前16个 int8: %s' % list(np.frombuffer(raw, dtype=np.int8)[:16]))
    print('   onnx  前16个原始 : %s' % np.round(w[:16], 4))
    print('   排序后前8: espdl %s | onnx %s' % (np.round(np.sort(a)[:8],2), np.round(np.sort(w)[:8],4)))
    print('')