import os, sys
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import inspect_espdl as I
Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
mode, payload = I.split_edl2(sys.argv[1])
model = Mdl.Model.GetRootAs(payload, 0)
g = model.Graph()
for i in range(g.InitializerLength()):
    t = g.Initializer(i)
    nm = I.decode(t.Name())
    dims = [t.Dims(j) for j in range(t.DimsLength())]
    try: dl = t.DataLocation()
    except Exception: dl = None
    try: rd = t.RawDataLength()
    except Exception: rd = None
    try: ext = t.ExternalDataLength()
    except Exception: ext = None
    try: ex = list(t.ExponentsAsNumpy()) if t.ExponentsLength() else []
    except Exception: ex = None
    if i < 8 or 'container' in nm:
        print('%-38s dims=%-18s dtype=%s loc=%s raw=%s ext=%s exp=%s' % (nm[:38], dims, t.DataType(), dl, rd, ext, ex))