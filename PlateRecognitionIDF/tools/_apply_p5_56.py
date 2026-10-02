# -*- coding: utf-8 -*-
"""P5.56: 兜底档加占屏下限 + ROI 日志字段改名 + 版本横幅 P5.56 (先全量校验锚点, 再一次性写入)"""
import io

SRC = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"

PATCHES = []
def P(old, new, tag): PATCHES.append((old, new, tag))

P(
'''static const float ROI_MAX_AREA_PCT = 45.0f;
''',
'''static const float ROI_MAX_AREA_PCT = 45.0f;
// P5.56: 兜底档的**占屏下限**。兜底档刻意不看形状(比例/填充), 面积是它唯一的底线 ——
//   而这条底线原来只有严格档那个 38 格(0.2%), 太低: 实测出现过 84x16 px(占屏 0.4%、长宽比 5.25)
//   的碎块被兜底收下, 喂给模型后吐出一个孤零零的省字(那次日志的 #196)。
//   取 2.0%: 能用的距离内真车牌至少占屏 4%(190x76 px, 见文档四十二节 档3 的 92%), 2% 留一倍余量,
//   既拦掉 0.1%~1.3% 那一带碎块, 又不至于把"稍远但还看得清"的牌误杀。只作用于兜底档, 严格档不变。
static const float ROI_FALLBACK_MIN_AREA_PCT = 2.0f;
''', "常量")

P(
'''    float c_dens[ROI_MAX_SLOTS], c_denseff[ROI_MAX_SLOTS], c_score[ROI_MAX_SLOTS];''',
'''    float c_dens[ROI_MAX_SLOTS], c_denseff[ROI_MAX_SLOTS], c_score[ROI_MAX_SLOTS];
    float c_area_pct[ROI_MAX_SLOTS];   // P5.56: 每个候选的占屏 %, 兜底档的占屏下限要用它''',
"c_area_pct 声明")

P(
'''        c_ratio[k] = c_axis_ratio[k] = c_dens[k] = c_denseff[k] = c_score[k] = 0.0f;''',
'''        c_ratio[k] = c_axis_ratio[k] = c_dens[k] = c_denseff[k] = c_score[k] = 0.0f;
        c_area_pct[k] = 0.0f;''', "c_area_pct 初始化")

P(
'''        if (area_pct_k > ROI_MAX_AREA_PCT) { c_capped[k] = true; c_why[k] = "占屏过大(疑似反光/背景连成一片)"; continue; }''',
'''        c_area_pct[k] = area_pct_k;   // P5.56: 存下来给兜底档用
        if (area_pct_k > ROI_MAX_AREA_PCT) { c_capped[k] = true; c_why[k] = "占屏过大(疑似反光/背景连成一片)"; continue; }''',
"c_area_pct 赋值")

P(
'''        for (int k = 0; k < nslot; k++) {
            if (slots[k].area < min_cells) continue;   // 只留"面积够大"这一条底线(与严格档同数), 不放开小碎点
            if (c_capped[k]) continue;                 // P5.49: 占屏过大的一律不收 —— 否则"兜底"会把整屏背景收下来
            if (c_score[k] > best2) { best2 = c_score[k]; best_k = k; }
        }''',
'''        int fb_too_small = 0;
        for (int k = 0; k < nslot; k++) {
            if (slots[k].area < min_cells) continue;   // 只留"面积够大"这一条底线(与严格档同数), 不放开小碎点
            if (c_capped[k]) continue;                 // P5.49: 占屏过大的一律不收 —— 否则"兜底"会把整屏背景收下来
            if (c_area_pct[k] < ROI_FALLBACK_MIN_AREA_PCT) { fb_too_small++; continue; }   // P5.56: 占屏太小也不收
            if (c_score[k] > best2) { best2 = c_score[k]; best_k = k; }
        }
        if (best_k < 0 && fb_too_small > 0) {
            ESP_LOGW(TAG, "兜底档也放弃: %d 个候选占屏 < %.1f%% (太小, 认不准, 宁可不报)", fb_too_small, ROI_FALLBACK_MIN_AREA_PCT);
        }''', "兜底档占屏下限")

P(
'''        ESP_LOGW(TAG, "本帧走兜底档: 面积>=%d格, 比例/填充**不看** (严格档是 比例%.1f~%.1f 填充>=%.0f%%)",
                 min_cells, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);''',
'''        ESP_LOGW(TAG, "本帧走兜底档: 面积>=%d格 且 占屏>=%.1f%%, 比例/填充**不看** (严格档是 比例%.1f~%.1f 填充>=%.0f%%)",
                 min_cells, ROI_FALLBACK_MIN_AREA_PCT, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);''',
"兜底档打印")

P(
'''            char covs[24] = "";
            if (cov_lo >= 0.0f && cov_hi > cov_lo)
                snprintf(covs, sizeof(covs), " 蓝带[%.0f%%,%.0f%%]", cov_lo * 100.0f, cov_hi * 100.0f);
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 收边[%.0f%%,%.0f%%]%s | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",''',
'''            // P5.56: 字段改名 —— 原先的 "收边[x%,y%]" 其实是**短边(上下)**的取样区间, 长边那个叫"蓝带",
            //   两个挨着打很容易看反(我自己第一眼就看错了)。现在:
            //     上下[上%,下%]  = 短边方向的取样区间 (v_lo/v_hi)
            //     蓝带[左%~右%]  = 蓝牌真正占定位框长边的那一段
            //     多吃[左%,右%]  = 框两端各自比蓝牌多吃了多少背景 (长边收边的直接依据)
            char covs[64] = "";
            if (cov_lo >= 0.0f && cov_hi > cov_lo)
                snprintf(covs, sizeof(covs), " 蓝带[%.0f%%~%.0f%%] 多吃[左%.0f%% 右%.0f%%]",
                         cov_lo * 100.0f, cov_hi * 100.0f,
                         cov_lo * 100.0f, (1.0f - cov_hi) * 100.0f);
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 上下[%.0f%%,%.0f%%]%s | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",''',
"ROI 行字段改名")

P(
'''        char rlx[32] = "";''',
'''        char rlx[48] = "";''', "rlx 缓冲放大")

P(
'''    ESP_LOGI(TAG, "    P5.43: 新增[蓝带]诊断 —— ROI 行里多了 蓝带[左%%,右%%], 表示定位框内蓝牌真正占的那一段; 它比框窄多少, 就是框多吃了多少背景");''',
'''    ESP_LOGI(TAG, "    P5.43/P5.56: ROI 行里的 [蓝带[左%%~右%%] 多吃[左%%,右%%]] = 蓝牌真正占定位框长边的那一段, 以及框两端各自多吃了多少背景");
    ESP_LOGI(TAG, "            [上下[上%%,下%%]] = 短边方向的取样区间 (P5.56 前这个字段误标成『收边』, 极易被当成长边那个收边看)");''',
"帮助文本 蓝带")

P(
'''    ESP_LOGI(TAG, "    P5.49: **候选加 %.0f%% 占屏上限**''',
'''    ESP_LOGI(TAG, "    P5.56: **兜底档加 %.1f%% 占屏下限** —— 兜底档只看面积, 原来 0.2%% 的底线太低, 84x16 px 这种碎块也会被收下并吐出一个孤零零的省字; 严格档不受影响", ROI_FALLBACK_MIN_AREA_PCT);
    ESP_LOGI(TAG, "    P5.49: **候选加 %.0f%% 占屏上限**''', "帮助文本 P5.56")

P(
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.55 ===");''',
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.56 ===");''', "版本横幅")

DRY = "--dry" in sys.argv if (sys := __import__("sys")) else False
s = io.open(SRC, encoding="utf-8", newline="").read()

bad = []
for old, new, tag in PATCHES:
    n = s.count(old)
    print("%-22s 命中 %d %s" % (tag, n, "OK" if n == 1 else "*** 不是 1 ***"))
    if n != 1: bad.append(tag)
if bad:
    print("\n有锚点不唯一, 未写入任何改动: %s" % bad); raise SystemExit(1)

n0 = len(s)
for old, new, tag in PATCHES:
    s = s.replace(old, new, 1)
io.open(SRC, "w", encoding="utf-8", newline="").write(s)
print("\n全部生效, 长度 %d -> %d" % (n0, len(s)))
