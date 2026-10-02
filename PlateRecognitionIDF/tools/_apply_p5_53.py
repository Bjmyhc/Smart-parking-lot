# -*- coding: utf-8 -*-
"""P5.53: 留一份实时输入, 跑完后再原样重放 -> 同一份字节, 两次结果是否一致。"""
import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'

A_OLD = '''        int64_t t_inf = esp_timer_get_time();
        model->run();'''
A_NEW = '''        // P5.53: 原样留一份"实时这一帧真正喂进去的字节" (run() 之后这块显存会被 esp-dl 挪作他用)
        memcpy(g_live_in, model_input->data, sizeof(g_live_in));

        int64_t t_inf = esp_timer_get_time();
        model->run();'''

B_OLD = '''        std::string probe_cmp_txt;
        if (!plate_looks_valid(plate)) {
            dl::TensorBase *pt = new dl::TensorBase(
                {1, IMG_H, IMG_W, 3}, (const void *)probe_crop_bin, 0, dl::DATA_TYPE_FLOAT);
            model_input->assign(pt);
            model->run();
            output_float->assign(model->get_outputs().begin()->second);
            probe_cmp_txt = greedy_decode((const float *)output_float->data);
            delete pt;
            ESP_LOGW(TAG, "#%d 同帧对照: 探针(已知能读对) -> %s | 本帧实时 -> %s",
                     fn, probe_cmp_txt.c_str(), plate.c_str());
        }'''

B_NEW = '''        if (!plate_looks_valid(plate)) {
            // (a) 把这一帧的实时输入原样重放 —— 同字节两次结果不同 => 模型根本没读这块内存
            {
                const size_t nb = (size_t)IMG_W * IMG_H * 3;
                for (size_t i = 0; i < nb; i++) g_live_f[i] = (float)g_live_in[i] / 128.0f;
                dl::TensorBase *lt = new dl::TensorBase(
                    {1, IMG_H, IMG_W, 3}, (const void *)g_live_f, 0, dl::DATA_TYPE_FLOAT);
                model_input->assign(lt);
                model->run();
                output_float->assign(model->get_outputs().begin()->second);
                const std::string replay = greedy_decode((const float *)output_float->data);
                delete lt;
                ESP_LOGW(TAG, "#%d 重放(本帧实时输入 %08X) -> %s | 本帧实时 -> %s", fn,
                         (unsigned)mem_fnv32(g_live_in, nb), replay.c_str(), plate.c_str());
            }
            // (b) 再跑一次已知能读对的探针, 确认此刻环境本身没坏
            {
                dl::TensorBase *pt = new dl::TensorBase(
                    {1, IMG_H, IMG_W, 3}, (const void *)probe_crop_bin, 0, dl::DATA_TYPE_FLOAT);
                model_input->assign(pt);
                model->run();
                output_float->assign(model->get_outputs().begin()->second);
                const std::string pp = greedy_decode((const float *)output_float->data);
                delete pt;
                ESP_LOGW(TAG, "#%d 同帧探针 -> %s", fn, pp.c_str());
            }
        }'''

D_OLD = 'extern const uint8_t probe_crop_bin[] asm("_binary_probe_crop_bin_start");'
D_NEW = D_OLD + '''
// P5.53: 实时那一帧喂进模型的原始 int8 字节 (run() 之前留档), 以及重放用的 float 版本
static int8_t g_live_in[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_live_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));'''

s = io.open(MAIN, encoding='utf-8', newline='').read()
for i, (old, new) in enumerate([(D_OLD, D_NEW), (A_OLD, A_NEW), (B_OLD, B_NEW), ('P5.52 ===', 'P5.53 ===')]):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')