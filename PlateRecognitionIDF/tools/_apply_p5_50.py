# -*- coding: utf-8 -*-
"""P5.50: 给固件加一个开机无条件自检 (老样张 + 探针裁块), 并嵌入 probe_crop.bin。"""
import io
import os

ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
MAIN = os.path.join(ROOT, 'main', 'main.cpp')
CMK = os.path.join(ROOT, 'main', 'CMakeLists.txt')


def edit(path, pairs):
    s = io.open(path, encoding='utf-8', newline='').read()
    for i, (old, new) in enumerate(pairs):
        n = s.count(old)
        if n != 1:
            raise SystemExit('%s: anchor %d matched %d times:\n%r' % (os.path.basename(path), i, n, old[:120]))
        s = s.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as fh:
        fh.write(s)
    print('patched %s' % path)


# ---------- CMakeLists: 多嵌一个 probe_crop.bin ----------
cmk_old = '''set(embed_files
    ${CMAKE_CURRENT_LIST_DIR}/models/lprnet_s3.espdl
    ${CMAKE_CURRENT_LIST_DIR}/models/test_input.bin)'''
cmk_new = '''set(embed_files
    ${CMAKE_CURRENT_LIST_DIR}/models/lprnet_s3.espdl
    ${CMAKE_CURRENT_LIST_DIR}/models/test_input.bin
    ${CMAKE_CURRENT_LIST_DIR}/models/probe_crop.bin)'''
edit(CMK, [(cmk_old, cmk_new)])

# ---------- main.cpp ----------
a_old = 'extern const uint8_t test_input_bin[] asm("_binary_test_input_bin_start");'
a_new = (a_old + '\n' +
         '// P5.50 自检探针: 用户实拍那一帧的 94x24 模型输入块 (板端自己发回来的), PC 端 float 读作 京Q06666\n'
         'extern const uint8_t probe_crop_bin[] asm("_binary_probe_crop_bin_start");')

b_old = 'extern "C" void app_main(void) {'
b_new = '''/** P5.50 自检探针: 让一张内嵌的 float32 裸张量走完整条"assign 量化 -> run -> CTC 解码",
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
}

''' + b_old

c_old = '        new dl::TensorBase(model_output->shape, nullptr, 0, dl::DATA_TYPE_FLOAT);'
c_new = (c_old + '\n\n' +
         '    // P5.50: 开机无条件跑一次内嵌样张自检 (原来只在「摄像头起不来」时才跑)。\n'
         '    //   两张图: 老样张当回归基线; 探针是 PC 端 float 读作 京Q06666 的那一帧裁块。\n'
         '    //   板端若也读出 京Q06666 -> 推理链路没问题, 问题在取样/发图; 板端若读出 京Q粤 -> 推理本身有问题。\n'
         '    run_probe_selfcheck(model, model_input, output_float, test_input_bin, "旧样张 沪AMS087");\n'
         '    run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针 板端发回的裁块");')

d_old = 'P5.49 ==='
d_new = 'P5.50 ==='

edit(MAIN, [(a_old, a_new), (b_old, b_new), (c_old, c_new), (d_old, d_new)])
print('done')