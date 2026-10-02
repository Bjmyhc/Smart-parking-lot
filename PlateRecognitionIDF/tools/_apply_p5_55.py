# -*- coding: utf-8 -*-
"""P5.55 临时诊断: 每帧强制发一次 94x24 裁块 + 每帧都跑探针对照。"""
import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'

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