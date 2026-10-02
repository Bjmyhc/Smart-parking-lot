# -*- coding: utf-8 -*-
"""_apply_p5_60.py —— 把 P5.60 的四处改动写进 main.cpp。
   全部用锚点(字符串), 不认行号; 先全量校验唯一性, 再一次性写入。
"""
import io, os, sys

P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
src = io.open(P, "r", encoding="utf-8", newline="").read()
orig_len = len(src)

EDITS = []

# ---------------- E1: 新常量 ----------------
EDITS.append((
"static const int ROI_MIN_VALUE = 120;\n",
"""static const int ROI_MIN_VALUE = 120;
// P5.60: 蓝牌判据换成"蓝通道明显高于红通道"。
//   依据 (2026-10-01, 用户新拍的 171 张原始帧: 京Q06666 / 豫FSQ818 / 豫A8F8Q8, 白天屏摄+反光):
//     只把蓝牌判据从"色相 180~270 + 饱和度>=35"换成 "(b-r)>40 且 b>120", 其余机器一字不动,
//     端到端整串全对 28/171 -> 96/171 (定位机器、比例/填充门槛、裁剪几何全部没变)。
//   为什么: 屏摄场景里反光/屏幕底色会把色相和饱和度一起搅乱(同一块牌的不同像素色相能差几十度),
//     而"蓝比红高多少"几乎不受影响 —— 蓝牌底 b-r 中位约 107, 屏幕泛蓝的底色只有 20~40。
//   注意 b > ROI_MIN_VALUE 这一半是给绿牌留的: 深绿像素的 b 只有 60~90, 过不了这条, 不会误进蓝牌分支。
static const int ROI_BLUE_BR_MIN = 40;
"""))

# ---------------- E2: 蓝牌提前返回 ----------------
EDITS.append((
"""    if (v < ROI_MIN_VALUE) return false;       // P5.49: 亮度下限 46 -> 120 (原来是 V >= 46)
    const int delta = v - m;
""",
"""    if (v < ROI_MIN_VALUE) return false;       // P5.49: 亮度下限 46 -> 120 (原来是 V >= 46)
    // P5.60: 蓝牌先走这条 —— 只有比较和减法, 没有除法也没有色相, 所以既更稳也更快
    //   (旧判据要算 s = 255*delta/v 和整数色相, 那是"定位 156 ms"里的主要开销)。
    if ((b - r) > ROI_BLUE_BR_MIN && b > ROI_MIN_VALUE) return true;
    const int delta = v - m;
"""))

# ---------------- E3: 旧蓝牌分支作废 ----------------
EDITS.append((
"""    if (deg >= 180 && deg <= 270) return s >= 35;   // 蓝牌 (P5.17: 由 [200,248]+S43 放宽)
""",
"""    // P5.60: 蓝牌已经在上面按 (b-r) 判过了, 走到这里说明这个像素"蓝得不明显" —— 不要。
    //   (旧规则 = 色相 180~270 且 s>=35, 屏摄时会把反光/屏幕泛蓝一起放进来。)
    if (deg >= 180 && deg <= 270) return false;
"""))

# ---------------- E4: 比例窗口 ----------------
EDITS.append((
"""static const float ROI_RATIO_LO = 2.0f;           // auto_crop_predict.py: ratio < 2.0 丢弃
static const float ROI_RATIO_HI = 6.5f;           // auto_crop_predict.py: ratio > 6.5 丢弃
""",
"""// P5.60: 2.0~6.5 -> 2.2~4.2。
//   真车牌本体(旋转拟合后)长宽比 3.1~3.4, 4.2 已经比它宽 25%, 够宽容了;
//   而 4.2~6.5 那一档收进来的全是"车牌 + 旁边一条蓝色背景"的连体块 —— 复算显示它对识别没贡献、只有干扰。
static const float ROI_RATIO_LO = 2.2f;
static const float ROI_RATIO_HI = 4.2f;
"""))

# ---------------- E5: 严格档贴边也照用 ----------------
EDITS.append((
"""                } else {
                    static char note[224];   // 中文按字节算, 80 字节不够 (GCC format-truncation 会直接报错)
                    snprintf(note, sizeof(note), "车牌贴到画面%s边缘被切掉了(也没有不贴边的候选可换) —— 把它完整移进画面, 四周留一成余量", edges);
                    g_roi_note = note;
                    if (verbose) {
                        ESP_LOGW(TAG, "选中的框贴到画面%s边缘, 且没有不贴边的替补 (色格%d 比例 %.2f 有效填充 %.0f%%) -> 拒绝出结果",
                                 edges, slots[best_k].area, c_ratio[best_k], c_denseff[best_k] * 100.0f);
                        static int edge_mask_left = 5;   // 掩码很贵, 只在前几次拒绝时打
                        if (edge_mask_left > 0) { edge_mask_left--; roi_dump_mask(cell, ROI_CELL_NEED, nullptr); }
                    }
                    return false;
                }
""",
"""                } else {
                    // P5.60: 原来这里是"整帧丢掉, 让用户把车牌移进画面"。
                    //   依据 (171 张原始帧复算): 判据修好之后, 贴边的那 9 帧里有 6 帧其实是**对的** ——
                    //   整帧丢掉是纯亏, 而且现场没法次次都把牌摆在画面中间。
                    //   真被画面切掉一半的那种, 后面的结果闸门(7~8 位 + 省字开头)仍会把乱码挡掉。
                    ESP_LOGW(TAG, "选中的框贴到画面%s边缘, 没有不贴边的替补 -> 照用 (老固件在这里整帧丢掉; 结果交给闸门判)",
                             edges);
                }
"""))

# ---------------- E6a: 饱和增强常量 ----------------
EDITS.append((
"""#define CROP_NORM_MIN_SPREAD 12          // 跨度小于它就别动(纯色块/噪声, 拉它没意义)
""",
"""#define CROP_NORM_MIN_SPREAD 12          // 跨度小于它就别动(纯色块/噪声, 拉它没意义)

// P5.60: 真正施加的增强 —— 整数饱和度倍数, 定点 8 位小数 (512 = x2.00)。
//   为什么把 P5.10 的"按亮度跨度拉伸"换掉 (2026-10-01, 用户新拍 171 张原始帧, 端到端复算):
//     原图裁(什么都不加)      93/171 (54%)
//     + 跨度拉伸(P5.10 做法)   80/171 (47%)   <- 在这批本身就拍得不差的屏摄图上, 它会拉过头
//     + 饱和度 x2.0           114/171 (67%)  <- 现在这个
//   标定: k 在 1.4~2.6 之间是平台(63%~67%), 取 2.0 居中, 不敏感。
//   位置: 在"缩放到 94x24 之后"做。离线验证过它与"整帧先增强再裁"结果完全一致(都是 114/171),
//     但整帧是 30 万像素、这里只有 2256 像素 —— 便宜一百倍以上, 所以不做整帧那一趟。
#define CROP_SAT_K256 512
#define CROP_SAT_GAIN ((float)CROP_SAT_K256 / 256.0f)
"""))

# ---------------- E6b: 逐像素变换 ----------------
EDITS.append((
"""        int b = g_crop_rgb[i * 3 + 0], g = g_crop_rgb[i * 3 + 1], r = g_crop_rgb[i * 3 + 2];
        if (s > 1.0f) {
            b = (int)(mb + (float)(b - (int)mb) * s + ((b > mb) ? 0.5f : -0.5f));
            g = (int)(mg + (float)(g - (int)mg) * s + ((g > mg) ? 0.5f : -0.5f));
            r = (int)(mr + (float)(r - (int)mr) * s + ((r > mr) ? 0.5f : -0.5f));
            if (b < 0) b = 0; else if (b > 255) b = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (r < 0) r = 0; else if (r > 255) r = 255;
        }
""",
"""        int b = g_crop_rgb[i * 3 + 0], g = g_crop_rgb[i * 3 + 1], r = g_crop_rgb[i * 3 + 2];
        // P5.60: 整数饱和度增强 —— 以"最亮通道"为原点, 把另外两个通道往外推, 色相不动:
        //     v = max(b,g,r);   out_c = v + ((c - v) * CROP_SAT_K256 >> 8)
        //   只有 3 次乘加 + 算术右移 + clamp, 没有除法/浮点/分支, 也不再需要三通道均值。
        //   (右移对负数是"向 -inf 取整", 与这里 int 的语义一致, 不需要额外补偿。)
        const int v = (b > g) ? ((b > r) ? b : r) : ((g > r) ? g : r);
        b = v + (((b - v) * CROP_SAT_K256) >> 8);
        g = v + (((g - v) * CROP_SAT_K256) >> 8);
        r = v + (((r - v) * CROP_SAT_K256) >> 8);
        if (b < 0) b = 0; else if (b > 255) b = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
"""))

# ---------------- E7: 两处日志标签 ----------------
EDITS.append((
"""                ESP_LOGI(TAG, "裁剪块质量: 亮度 p5=%d p95=%d 跨度=%d | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 对比度归一 x%.2f",
                         g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.spread, g_crop_q.over_pct,
                         g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                         g_crop_q.norm_s);
""",
"""                ESP_LOGI(TAG, "裁剪块质量: 亮度 p5=%d p95=%d 跨度=%d | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 饱和度 x%.2f",
                         g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.spread, g_crop_q.over_pct,
                         g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                         CROP_SAT_GAIN);
"""))
EDITS.append((
"""                        ESP_LOGI(TAG, "#%d 质量: 亮度跨度 %d (p5=%d p95=%d) | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 对比度归一 x%.2f",
                                 fn, g_crop_q.spread, g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.over_pct,
                                 g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                                 g_crop_q.norm_s);
""",
"""                        ESP_LOGI(TAG, "#%d 质量: 亮度跨度 %d (p5=%d p95=%d) | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 饱和度 x%.2f",
                                 fn, g_crop_q.spread, g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.over_pct,
                                 g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                                 CROP_SAT_GAIN);
"""))

# ---------------- E8: 版本号 + 说明行 ----------------
EDITS.append((
"""    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.59 ===");
""",
"""    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.60 ===");
"""))

# ---------------- 校验 + 写入 ----------------
ok = True
for i, (old, new) in enumerate(EDITS):
    n = src.count(old)
    if n != 1:
        print("!! 锚点 %d 命中 %d 次 (要求 1 次)" % (i, n))
        print("   " + old.strip().split("\n")[0][:100])
        ok = False
if not ok:
    print("校验失败, 未写入任何内容")
    sys.exit(1)

for old, new in EDITS:
    src = src.replace(old, new, 1)

if "\r" in src:
    print("!! 结果里出现了 CR, 原文件是纯 LF, 中止")
    sys.exit(1)

io.open(P, "w", encoding="utf-8", newline="").write(src)
print("已写入 %s" % P)
print("字节 %d -> %d  (+%d)" % (orig_len, len(src), len(src) - orig_len))
