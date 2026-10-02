# -*- coding: utf-8 -*-
"""P5.52: 摄像头打开后再跑一次已知能读对的探针 -> 分辨"数据坏"还是"环境坏"。"""
import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'

A_OLD = '        ESP_LOGI(TAG, "摄像头就绪: VGA RGB565, XCLK 20MHz, fb_count=2");'
A_NEW = '''        ESP_LOGI(TAG, "摄像头就绪: VGA RGB565, XCLK 20MHz, fb_count=2");
        // P5.52: 开机自检时探针读对, 摄像头一起来就变错 -> 说明是运行时内存/环境被搅了, 不是数据问题
        run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针/摄像头已开第1次");
        run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针/摄像头已开第2次");'''

B_OLD = '        std::string plate = greedy_decode_impl((const float *)output_float->data, &rep);'
B_NEW = B_OLD + '''
        // P5.52: 同帧对照 —— 实时结果是坏的时, 立刻用"已知能读对的探针"再跑一次同一条链路。
        //   实时坏 + 探针也坏  => 环境/内存问题 (和数据无关)
        //   实时坏 + 探针还好  => 这一帧喂进去的数据确实和发出来的裁块不一样
        bool probe_cmp_done = false;
        std::string probe_cmp_txt;
        if (!plate_looks_valid(plate)) {
            dl::TensorBase *pt = new dl::TensorBase(
                {1, IMG_H, IMG_W, 3}, (const void *)probe_crop_bin, 0, dl::DATA_TYPE_FLOAT);
            model_input->assign(pt);
            model->run();
            output_float->assign(model->get_outputs().begin()->second);
            probe_cmp_txt = greedy_decode((const float *)output_float->data);
            delete pt;
            probe_cmp_done = true;
        }'''

s = io.open(MAIN, encoding='utf-8', newline='').read()
for i, (old, new) in enumerate([(A_OLD, A_NEW), (B_OLD, B_NEW), ('P5.51 ===', 'P5.52 ===')]):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')