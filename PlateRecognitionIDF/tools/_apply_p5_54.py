# -*- coding: utf-8 -*-
"""P5.54: 在"裁块点"和"run() 前"各量一次输入缓冲校验和, 看中间有没有人动过。"""
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

A_OLD = '        // P5.8: 裁剪块的三通道均值 —— 必须在这里取(run() 之后这块显存会被复用, 见下面那段警告)。'
A_NEW = '''        // P5.54: 裁块点的缓冲校验和 —— 和 run() 前那一次比对, 就能知道中间有没有人动过输入
        const uint32_t ck_crop = mem_fnv32(model_input->data, (size_t)IMG_W * IMG_H * 3);

''' + A_OLD

B_OLD = '''        // P5.53: 原样留一份"实时这一帧真正喂进去的字节" (run() 之后这块显存会被 esp-dl 挪作他用)
        memcpy(g_live_in, model_input->data, sizeof(g_live_in));
'''
B_NEW = '''        // P5.53: 原样留一份"实时这一帧真正喂进去的字节" (run() 之后这块显存会被 esp-dl 挪作他用)
        memcpy(g_live_in, model_input->data, sizeof(g_live_in));
        const uint32_t ck_pre = mem_fnv32(g_live_in, sizeof(g_live_in));
        ESP_LOGI(TAG, "#%d 输入缓冲: 裁块点=%08X run前=%08X %s", fn, (unsigned)ck_crop, (unsigned)ck_pre,
                 (ck_crop == ck_pre) ? "一致" : "*** 中间被改过 ***");
'''
s = io.open(MAIN, encoding='utf-8', newline='').read()
for i, (old, new) in enumerate([(A_OLD, A_NEW), (B_OLD, B_NEW), ('P5.53 ===', 'P5.54 ===')]):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')