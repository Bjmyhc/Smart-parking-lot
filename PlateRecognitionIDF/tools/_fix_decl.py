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

bad = '''// P5.53: 实时那一帧喂进模型的原始 int8 字节 (run() 之前留档), 以及重放用的 float 版本
static int8_t g_live_in[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_live_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
'''
assert s.count(bad) == 1
s = s.replace(bad, '')

anchor = 'static const int TIME_STEPS = 18;'
assert s.count(anchor) == 1
add = anchor + '''
// P5.53: 实时那一帧喂进模型的原始 int8 字节 (run() 之前留档), 以及重放用的 float 版本
static int8_t g_live_in[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_live_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static uint32_t mem_fnv32(const void *p, size_t n);   // 定义在后面, 先用先声明'''
s = s.replace(anchor, add)

with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('fixed')