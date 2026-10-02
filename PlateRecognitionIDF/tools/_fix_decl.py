# -*- coding: utf-8 -*-
import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'
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