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

import sys, os, struct, hashlib, glob
ROOT = _REPO_ROOT_ + r'\PlateRecognitionIDF'
blob = open(sys.argv[1],'rb').read()
out_dir = sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
i = 0; n = 0
while True:
    j = blob.find(b'EDL2', i)
    if j < 0: break
    mode, size, pad = struct.unpack('<III', blob[j+4:j+16])
    if 100000 < size < 4000000 and j + 16 + size <= len(blob):
        data = blob[j:j+16+size]
        out = os.path.join(out_dir, 'flash_model_%d_%d.espdl' % (j, size))
        open(out,'wb').write(data)
        print('提取 %s  size=%d sha1=%s' % (out, size, hashlib.sha1(data).hexdigest()[:16]))
        n += 1
    i = j + 1
print('共提取 %d 个候选' % n)
print('')
print('%-58s %9s %s' % ('已有 espdl 文件', 'size', 'sha1(前16)'))
for p in sorted(glob.glob(os.path.join(ROOT, '**', '*.espdl'), recursive=True)):
    d = open(p,'rb').read()
    rel = os.path.relpath(p, ROOT)
    print('%-58s %9d %s' % (rel, len(d), hashlib.sha1(d).hexdigest()[:16]))