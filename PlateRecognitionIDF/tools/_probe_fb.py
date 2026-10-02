import os, sys
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import inspect_espdl as I
Mdl = I.load_flatbuffers(I.DEFAULT_PPQ_SITE)
mode, payload = I.split_edl2(sys.argv[1])
model = Mdl.Model.GetRootAs(payload, 0)
g = model.Graph()
print('inits', g.InitializerLength())
t = g.Initializer(0)
print('methods', [x for x in dir(t) if not x.startswith('_')])
print('name', I.decode(t.Name()), 'dims', [t.Dims(j) for j in range(t.DimsLength())])
for meth in ['RawDataAsNumpy','DataAsNumpy','Int8DataAsNumpy','Uint8DataAsNumpy','FloatDataAsNumpy','DataLength','Quantization']:
    try:
        v = getattr(t, meth)()
        print(meth, type(v), getattr(v,'shape',None), getattr(v,'dtype',None))
    except Exception as e:
        print(meth, 'ERR', e)