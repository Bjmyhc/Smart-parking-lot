# -*- coding: utf-8 -*-
"""_apply_p5_60b.py —— 开机横幅加 P5.60 说明行。"""
import io, sys
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
src = io.open(P, "r", encoding="utf-8", newline="").read()
old = '''    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.49 才说明烧进去的是新固件", __DATE__, __TIME__);
'''
new = '''    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.60 才说明烧进去的是新固件", __DATE__, __TIME__);
    ESP_LOGI(TAG, "    P5.60: **蓝牌判据换成 (b-r)>40 且 b>120; 比例窗 2.0~6.5 -> 2.2~4.2; 严格档贴边也照用; 裁剪块的[按亮度跨度拉伸]换成[整数饱和度 x2.0]**");
    ESP_LOGI(TAG, "           依据: 用户新拍 171 张原始帧(京Q06666/豫FSQ818/豫A8F8Q8), PC 端到端复算 整串全对 28/171 -> 114/171");
    ESP_LOGI(TAG, "           拆开看: 只换蓝牌判据 28->96 | 再换裁剪增强 +12 | 贴边不再整帧丢 +6 | 比例窗 +1");
'''
n = src.count(old)
if n != 1:
    print("!! 锚点命中 %d 次" % n); sys.exit(1)
src = src.replace(old, new, 1)
if "\r" in src:
    print("!! 出现 CR"); sys.exit(1)
io.open(P, "w", encoding="utf-8", newline="").write(src)
print("已写入横幅说明行")
