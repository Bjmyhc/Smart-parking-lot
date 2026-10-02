# -*- coding: utf-8 -*-
"""_apply_p5_61.py -- 按锚点精确改 main.cpp (P5.61): 关掉长边收边 + 左留白+2% + 上下留白+11%."""
import io, sys, os
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
src = io.open(P, encoding="utf-8", newline="").read()
orig_len = len(src)

PATCHES = []

PATCHES.append((
"""// P5.34/P5.59: "长边收边"开关。开 = 取样框**左端**按实测蓝色边界收进来 (修 CTC 在左边多吐一个省字); 关 = 回到 P5.33 行为。
//   BOOT 双击切换 (P5.35 起; 原先是 1~2s 中按, 用户反馈太容易误触)。默认开 —— 34 帧影子对照: 省字对 59% -> 76%。
static bool g_utrim_on = true;""",
"""// P5.34/P5.59: "长边收边"开关。开 = 取样框**左端**按实测蓝色边界收进来; 关 = 取样框保持定位框原样。
//   BOOT 双击切换 (P5.35 起; 原先是 1~2s 中按, 用户反馈太容易误触)。
// P5.61 定案: **默认关**。171 张原始帧按固件真流程复算(P5.60 那套 b-r 判据 + 四边形采样 + 饱和度 x2
//   + r4 模型): 收左端 76/171 = 44%, 不收 128/171 = 75%; 而且"收左端对"的 76 帧**完全**是"不收对"的
//   128 帧的子集 —— 也就是说收边 0 帧有帮助、52 帧帮倒忙。
//   原因: P5.60 换了 b-r 判据之后定位框本来就贴着车牌, 蓝带量出来的"左端那一段不是蓝"其实就是
//   **省字自己**(省字笔画太密, 一行 9 个采样点里蓝点常常不到 4 个), 再按它收 = 把省字切掉。
//   真机表现就是 京 -> 皖/粤/沪 乱跳, 或者整串少一个字。
//   双击仍能现场开回来做 A/B, 但它现在明确是"更差的那一边"。
static bool g_utrim_on = false;"""))

PATCHES.append((
"""    {ROI_PAD_U_L_OFF, ROI_PAD_U_R_ON},    // 档2 左净 0%  / 右净 +7%  <- P5.43 默认""",
"""    {ROI_PAD_U_L_ON,  ROI_PAD_U_R_ON},    // 档2 左净 +2% / 右净 +7%  <- P5.61 默认"""))

PATCHES.append((
"""//   结论: 左边要 小 (防多字), 右边要 大 (防手持抖动把末尾字符切出窗)。""",
"""//   结论: 左边要 小 (防多字), 右边要 大 (防手持抖动把末尾字符切出窗)。
// P5.61: 左边留白 净 0% -> 净 +2% (档2 的左端从 ROI_PAD_U_L_OFF 换成 ROI_PAD_U_L_ON)。
//   171 帧原始帧复算(同时把长边收边关掉): 左 +1% 128/171 -> 左 +3% **138/171 = 81%** -> 左 +5% 117/171;
//   而左 -1% 只有 68/171。左端**必须**留白, 3% 附近是个清晰的峰 —— 与 P5.38 的结论同向。"""))

PATCHES.append((
"""static const float ROI_PAD_V   = 0.08f;    // 短边两端 (净 5%, 维持 P5.13/P5.32 的结论)""",
"""// P5.61: 0.08 -> 0.12 (净 5% -> 净 11%)。同一批复算: V 0.05 -> 134/171, 0.08 -> 138/171,
//   0.12 -> **142/171 = 84%**, 0.15/0.18/0.22 -> 140/139/141 —— 0.12~0.22 是一条平顶,
//   取 0.12(实测最好); 上下多留一点对"牌顶被反光吃掉"也更宽容。
static const float ROI_PAD_V   = 0.12f;"""))

PATCHES.append((
"""    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.60 ===");""",
"""    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.61 ===");"""))

PATCHES.append((
"""    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.60 才说明烧进去的是新固件", __DATE__, __TIME__);""",
"""    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.61 才说明烧进去的是新固件", __DATE__, __TIME__);
    ESP_LOGI(TAG, "    P5.61: **长边收边改成默认关 + 左留白 净0%%->净+2%% + 上下留白 净5%%->净11%%** —— 171 帧原始帧按固件真流程复算: 整串全对 44%% -> 84%%, 省字对 91%%");"""))

PATCHES.append((
"""                    ESP_LOGW(TAG, "左端收边(P5.59): %s —— %s", g_utrim_on ? "开" : "关",
                             g_utrim_on ? "取样框左端按实测蓝色边界收进来, 少认左边多出来的省字"
                                        : "取样框维持定位框原样 (回到 P5.33 行为)");""",
"""                    ESP_LOGW(TAG, "左端收边(P5.61, 默认关): %s —— %s", g_utrim_on ? "开" : "关",
                             g_utrim_on ? "警告: 会切掉省字(171 帧复算 44% vs 75%), 只留着做 A/B"
                                        : "取样框维持定位框原样 (171 帧复算 128/171 = 75%)");"""))

PATCHES.append((
"""    ESP_LOGI(TAG, "    P5.59: **长边收边定案 = 只收左端(右端留白)**""",
"""    ESP_LOGI(TAG, "    P5.59(已在 P5.61 废弃): **长边收边定案 = 只收左端(右端留白)**"""))

bad = 0
for i, (old, new) in enumerate(PATCHES):
    n = src.count(old)
    if n != 1:
        print("ANCHOR %d count=%d -> ABORT" % (i, n))
        bad += 1
if bad:
    sys.exit(1)
for i, (old, new) in enumerate(PATCHES):
    src = src.replace(old, new, 1)
io.open(P, "w", encoding="utf-8", newline="").write(src)
print("OK patches=%d  %d -> %d bytes" % (len(PATCHES), orig_len, len(src)))
