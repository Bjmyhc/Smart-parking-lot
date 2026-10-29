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
sys.path.insert(0, _REPO_ROOT_ + r'\PlateRecognitionIDF\tools')
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