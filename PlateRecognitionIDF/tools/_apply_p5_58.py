# -*- coding: utf-8 -*-
"""P5.58: 把"占屏下限"从只作用于兜底档, 改成**严格档也生效** ——
   实测 76x16 px(占屏0.4%)的横条能过严格档的比例(4.75)+填充门槛, 被当候选喂给模型。
   常量随之改名 ROI_FALLBACK_MIN_AREA_PCT -> ROI_MIN_AREA_PCT。"""
import io
SRC = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
PATCHES = []
def P(old, new, tag): PATCHES.append((old, new, tag))

P(
'''// P5.56: 兜底档的**占屏下限**。兜底档刻意不看形状(比例/填充), 面积是它唯一的底线 ——
//   而这条底线原来只有严格档那个 38 格(0.2%), 太低: 实测出现过 84x16 px(占屏 0.4%、长宽比 5.25)
//   的碎块被兜底收下, 喂给模型后吐出一个孤零零的省字(那次日志的 #196)。
//   取 2.0%: 能用的距离内真车牌至少占屏 4%(190x76 px, 见文档四十二节 档3 的 92%), 2% 留一倍余量,
//   既拦掉 0.1%~1.3% 那一带碎块, 又不至于把"稍远但还看得清"的牌误杀。只作用于兜底档, 严格档不变。
static const float ROI_FALLBACK_MIN_AREA_PCT = 2.0f;''',
'''// P5.56/P5.58: **占屏下限**, 严格档和兜底档都生效。面积上下限是一对, 放在一起看:
//   上限 45% 治"整屏背景被收下", 下限治"碎块被当成车牌候选"。
//   原来的下限只有 38 格(0.2%), 太低 —— 实测两种碎块都能溜过去:
//     兜底档: 84x16 px(占屏 0.4%、长宽比 5.25)   -> 模型吐出一个孤零零的省字
//     严格档: 76x16 px(占屏 0.4%、长宽比 4.75)   -> 比例和填充居然都过, 同样吐一个省的
//   取 2.0%: 能用的距离内真车牌至少占屏 4%(190x76 px, 见文档四十二节 档3 的 92%), 2% 留一倍余量,
//   既拦掉 0.1%~1.3% 那一带碎块, 又不至于把"稍远但还看得清"的牌误杀。
static const float ROI_MIN_AREA_PCT = 2.0f;''', "常量改名")

P(
'''        c_area_pct[k] = area_pct_k;   // P5.56: 存下来给兜底档用
        if (area_pct_k > ROI_MAX_AREA_PCT) { c_capped[k] = true; c_why[k] = "占屏过大(疑似反光/背景连成一片)"; continue; }''',
'''        c_area_pct[k] = area_pct_k;   // P5.56: 存下来给兜底档用
        if (area_pct_k > ROI_MAX_AREA_PCT) { c_capped[k] = true; c_why[k] = "占屏过大(疑似反光/背景连成一片)"; continue; }
        // P5.58: 占屏下限, 严格档也拦 —— 见 ROI_MIN_AREA_PCT 的说明
        if (area_pct_k < ROI_MIN_AREA_PCT) { c_why[k] = "占屏太小(不可能是车牌)"; continue; }''',
"严格档下限")

P(
'''            if (c_area_pct[k] < ROI_FALLBACK_MIN_AREA_PCT) { fb_too_small++; continue; }   // P5.56: 占屏太小也不收''',
'''            if (c_area_pct[k] < ROI_MIN_AREA_PCT) { fb_too_small++; continue; }   // P5.56: 占屏太小也不收''',
"兜底档改用新名")

P(
'''            ESP_LOGW(TAG, "兜底档也放弃: %d 个候选占屏 < %.1f%% (太小, 认不准, 宁可不报)", fb_too_small, ROI_FALLBACK_MIN_AREA_PCT);''',
'''            ESP_LOGW(TAG, "兜底档也放弃: %d 个候选占屏 < %.1f%% (太小, 认不准, 宁可不报)", fb_too_small, ROI_MIN_AREA_PCT);''',
"兜底档打印")

P(
'''                 min_cells, ROI_FALLBACK_MIN_AREA_PCT, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);''',
'''                 min_cells, ROI_MIN_AREA_PCT, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);''',
"兜底档打印实参")

P(
'''    ESP_LOGI(TAG, "    P5.56: **兜底档加 %.1f%% 占屏下限** —— 兜底档只看面积, 原来 0.2%% 的底线太低, 84x16 px 这种碎块也会被收下并吐出一个孤零零的省字; 严格档不受影响", ROI_FALLBACK_MIN_AREA_PCT);''',
'''    ESP_LOGI(TAG, "    P5.56/P5.58: **占屏下限 %.1f%%** —— 上下限成对: 45%% 治整屏背景, %.1f%% 治碎块。原来底线只有 0.2%%, 84x16 / 76x16 px 这种横条两条路都能溜过去(比例+填充居然都合格), 喂给模型必吐乱码", ROI_MIN_AREA_PCT, ROI_MIN_AREA_PCT);''',
"帮助文本")

P(
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.57 ===");''',
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.58 ===");''', "版本横幅")

s = io.open(SRC, encoding="utf-8", newline="").read()
bad = []
for old, new, tag in PATCHES:
    n = s.count(old)
    print("%-16s 命中 %d %s" % (tag, n, "OK" if n == 1 else "*** 不是 1 ***"))
    if n != 1: bad.append(tag)
if bad: print("\n锚点不唯一, 未写入: %s" % bad); raise SystemExit(1)
n0 = len(s)
for old, new, tag in PATCHES: s = s.replace(old, new, 1)
io.open(SRC, "w", encoding="utf-8", newline="").write(s)
print("\n全部生效, %d -> %d" % (n0, len(s)))
assert s.count("ROI_FALLBACK_MIN_AREA_PCT") == 0, "还有旧名残留"
print("旧常量名已无残留")
