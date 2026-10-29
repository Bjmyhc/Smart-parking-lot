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

# -*- coding: utf-8 -*-
import io
MAIN = _REPO_ROOT_ + r'\PlateRecognitionIDF\main\main.cpp'
s = io.open(MAIN, encoding='utf-8', newline='').read()
pairs = [
 ('        bool probe_cmp_done = false;\n        std::string probe_cmp_txt;\n', '        std::string probe_cmp_txt;\n'),
 ('            probe_cmp_done = true;\n        }',
  '            ESP_LOGW(TAG, "#%d 同帧对照: 探针(已知能读对) -> %s | 本帧实时 -> %s",\n'
  '                     fn, probe_cmp_txt.c_str(), plate.c_str());\n        }'),
]
for i, (old, new) in enumerate(pairs):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')