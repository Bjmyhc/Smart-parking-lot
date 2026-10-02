# -*- coding: utf-8 -*-
"""P5.51: 自检里加"直写 int8"(复现实时链路) + 输入缓冲完整性校验。"""
import io, os

MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'

OLD = '''/** P5.50 自检探针: 让一张内嵌的 float32 裸张量走完整条"assign 量化 -> run -> CTC 解码",
 *  用来分辨到底是「板端推理链路」还是「取样/发图链路」把结果读坏的。 */
static void run_probe_selfcheck(dl::Model *model,
                                dl::TensorBase *model_input,
                                dl::TensorBase *output_float,
                                const uint8_t *blob,
                                const char *label) {
    dl::TensorBase *input_tensor = new dl::TensorBase(
        {1, IMG_H, IMG_W, 3}, (const void *)blob, 0, dl::DATA_TYPE_FLOAT);
    model_input->assign(input_tensor);   // 内部完成 float -> int8 量化
    int64_t t = esp_timer_get_time();
    model->run();
    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGW(TAG, ">>> 自检 [%s] -> %s   (%lld ms)", label, plate.c_str(),
             (long long)((esp_timer_get_time() - t) / 1000));
    delete input_tensor;
}'''

NEW = '''/** 简版 FNV-1a: 只回答"这块内存有没有被改过" */
static uint32_t mem_fnv32(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void log_input_tensor(const char *when, dl::TensorBase *t) {
    ESP_LOGW(TAG, "输入张量[%s]: data=%p shape=%s dtype=%s exp=%d", when, t->data,
             dl::vector_to_string(t->shape).c_str(), t->get_dtype_string(), t->exponent);
}

/** P5.50 自检探针: 一张内嵌的 float32 裸张量走"assign 量化 -> run -> CTC 解码"。
 *  P5.51: 顺带校验输入缓冲在 run() 前后有没有被覆盖 (esp-dl 会把输入显存还给池子)。 */
static void run_probe_selfcheck(dl::Model *model,
                                dl::TensorBase *model_input,
                                dl::TensorBase *output_float,
                                const uint8_t *blob,
                                const char *label) {
    dl::TensorBase *input_tensor = new dl::TensorBase(
        {1, IMG_H, IMG_W, 3}, (const void *)blob, 0, dl::DATA_TYPE_FLOAT);
    model_input->assign(input_tensor);   // 内部完成 float -> int8 量化
    log_input_tensor(label, model_input);
    const size_t nb = (size_t)IMG_W * IMG_H * 3;
    const uint32_t ck0 = mem_fnv32(model_input->data, nb);
    int64_t t = esp_timer_get_time();
    model->run();
    const uint32_t ck1 = mem_fnv32(model_input->data, nb);
    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGW(TAG, ">>> 自检 [%s] -> %s   (%lld ms) | 输入校验 %08X -> %08X %s", label, plate.c_str(),
             (long long)((esp_timer_get_time() - t) / 1000), ck0, ck1,
             (ck0 == ck1) ? "(run 没动过输入)" : "*** run 之后输入被覆盖了 ***");
    delete input_tensor;
}

/** P5.51: 复现实时链路 —— 不走 assign(), 直接把 int8 写进 model_input->data
 *  (和 sample_quad_to_input 干的事一样)。若这条是坏的、上面那条是好的,
 *  说明问题出在"往哪块内存写 / 那块内存 run() 期间归谁"。 */
static void run_probe_direct_int8(dl::Model *model,
                                  dl::TensorBase *model_input,
                                  dl::TensorBase *output_float,
                                  const uint8_t *blob,
                                  const char *label) {
    const float *f = (const float *)blob;
    const size_t nb = (size_t)IMG_W * IMG_H * 3;
    int8_t *d = (int8_t *)model_input->data;
    for (size_t i = 0; i < nb; i++) {
        int v = (int)floorf(f[i] * 128.0f + 0.5f);
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        d[i] = (int8_t)v;
    }
    log_input_tensor(label, model_input);
    const uint32_t ck0 = mem_fnv32(d, nb);
    int64_t t = esp_timer_get_time();
    model->run();
    const uint32_t ck1 = mem_fnv32(d, nb);
    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGW(TAG, ">>> 直写int8自检 [%s] -> %s   (%lld ms) | 输入校验 %08X -> %08X %s", label, plate.c_str(),
             (long long)((esp_timer_get_time() - t) / 1000), ck0, ck1,
             (ck0 == ck1) ? "(run 没动过输入)" : "*** run 之后输入被覆盖了 ***");
}'''

CALL_OLD = '''    run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针 板端发回的裁块");'''
CALL_NEW = '''    run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针 板端发回的裁块");
    // P5.51: 同一条数据, 换成"和实时链路一模一样的直写 int8"再跑一次
    run_probe_direct_int8(model, model_input, output_float, probe_crop_bin, "探针/直写int8");
    run_probe_direct_int8(model, model_input, output_float, test_input_bin, "旧样张/直写int8");'''

s = io.open(MAIN, encoding='utf-8', newline='').read()
for i, (old, new) in enumerate([(OLD, NEW), (CALL_OLD, CALL_NEW), ('P5.50 ===', 'P5.51 ===')]):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')