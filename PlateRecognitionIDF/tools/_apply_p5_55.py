# -*- coding: utf-8 -*-
"""P5.55 临时诊断: 每帧强制发一次 94x24 裁块 + 每帧都跑探针对照。"""
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

import io
MAIN = _REPO_ROOT_ + r'\PlateRecognitionIDF\main\main.cpp'

A_OLD = '    // P5.12: 不管有没有定位到车牌, 都把这一帧(缩一半)编码成 JPEG 发给串口助手预览。'
A_NEW = '''    // P5.55 临时诊断: 每帧都编一张 94x24 的 JPEG 发出去 —— 专门验"发图这条路"会不会搅坏模型
    img_tx_send_crop();

''' + A_OLD

B_OLD = '        if (!plate_looks_valid(plate)) {'
B_NEW = '        if (true) {   // P5.55 临时: 诊断期每帧都跑对照'

s = io.open(MAIN, encoding='utf-8', newline='').read()
for i, (old, new) in enumerate([(A_OLD, A_NEW), (B_OLD, B_NEW), ('P5.54 ===', 'P5.55 ===')]):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')