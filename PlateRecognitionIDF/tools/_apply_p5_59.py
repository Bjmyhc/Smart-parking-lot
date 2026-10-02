# -*- coding: utf-8 -*-
"""P5.59: 长边收边定案 = **只收左端(右端留白)**; 删掉 P5.57 的影子对照(结论已拿到, 换回速度)。
   依据: 2026-10-01 19:03 日志 34 帧 京Q06666 影子对照 A/B/C:
     现状 整串47%/省字59%; 两端收 47%/76%; 只收左 50%/76%
   两端收 在 #642/#645 会把末尾字切掉(多认一个 6) -> 淘汰; 只收左 只输一帧(#697)。"""
import io
SRC = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
PATCHES = []
def P(old, new, tag): PATCHES.append((old, new, tag))

# ---------------- E1: 删影子缓冲 ----------------
P(
'''// P5.57 影子对照: 两个"长边收边"变体各裁一份 int8 模型输入, 以及跑模型用的 float 中转。
//   每帧复用, 不参与任何报告状态 —— 报告结果永远是"现状"那条链路算出来的。
static int8_t g_shadow_i8[2][IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_shadow_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static int    g_shadow_n = 0;
''',
'',
"E1 影子缓冲")

# ---------------- E2: 删 shadow_run ----------------
P(
'''/**
 * P5.57 影子对照用: 把一个"影子裁块"(int8, 与实时链路同一套量化) 跑通模型, 返回识别串。
 * 只读 src、只借用 model_input/output_float; 不改变任何报告状态 —— 下一次 run() 从输入重算。
 * int8 -> float -> assign() 这条中转是 P5.51 验证过的路子 (实测与"直写 int8"逐字节等价)。
 */
static std::string shadow_run(dl::Model *model, dl::TensorBase *model_input,
                              dl::TensorBase *output_float, const int8_t *src) {
    const size_t nb = (size_t)IMG_W * IMG_H * 3;
    for (size_t i = 0; i < nb; i++) g_shadow_f[i] = (float)src[i] / 128.0f;
    dl::TensorBase *t = new dl::TensorBase(
        {1, IMG_H, IMG_W, 3}, (const void *)g_shadow_f, 0, dl::DATA_TYPE_FLOAT);
    model_input->assign(t);
    model->run();
    output_float->assign(model->get_outputs().begin()->second);
    const std::string r = greedy_decode((const float *)output_float->data);
    delete t;
    return r;
}

''',
'',
"E2 shadow_run")

# ---------------- E3: 常量 ----------------
P(
'''// P5.34: 长边(u)方向收边的守卫。实测 12 帧里单端最多收 6/24=25%, 两端合计最多 7/24=29%;
//   这两个门槛给到 30% / 留 60%, 正好放行实测数据、又能挡住"把整块牌收没"的异常情况。
#define ROI_UTRIM_MAX 0.30f
#define ROI_UTRIM_MIN_KEEP 0.60f
// P5.37: 收边的"扳机"和"下限"。实测 (2026-09-30, 41 帧两块牌):
//   框偏宽时收边净赚 (豫A1890P: 框 4.0~4.5 -> 收边开 16/18 对, 关 2/10);
//   框偏高时收边帮倒忙 (豫FSQ818: 框 2.4~2.5 -> 收边开 3/5, 关 8/8) —— 因为收边只修左右, 不修上下。
//   所以只在"框明显比真车牌宽"时才动手; 收完如果反而比真车牌还窄, 说明边界找错了, 整块不收。
#define ROI_UTRIM_TRIGGER 3.40f
#define ROI_UTRIM_FLOOR   3.00f
''',
'''// P5.59: 长边收边**定案 = 只收左端, 右端一律留白**。
//   怎么定的 (2026-10-01 19:03 日志, 34 帧 京Q06666, 影子对照 现状 / 两端收 / 只收左):
//     变体    整串全对    省字对
//     现状    16 (47%)   20 (59%)
//     两端收  16 (47%)   26 (76%)
//     只收左  17 (50%)   26 (76%)
//   两端收 与 只收左 的"省字对"打平, 但两端收在 #642/#645 把末尾字切掉(多认一个 6);
//   只收左 只输一帧 (#697), 而且"右端留白"本来就是防末尾字被切出窗的老办法 -> 选只收左。
//   P5.37 的扳机(3.40)/下限(3.00) 一并废掉: "京 -> 皖"这个省字错来自**左端多吃了背景**,
//   跟框的宽高比没有关系 (比例 2.73 的 #643 也多吃), 门槛留着只会白放过一批该收的帧。
#define ROI_UTRIM_LEFT_MAX 0.45f   // 左端最多收 45%; 再多就是边界量错了 -> 这一帧不收
#define ROI_UTRIM_BAND_MIN 0.60f   // 蓝带跨度 < 60% => 边界量错了 -> 这一帧不收
''',
"E3 常量")

# ---------------- E4: 函数说明 ----------------
P(
''' * 同一套"收边"逻辑短边方向早就在用 (roi_trim_short_axis), 这里照它的骨架把方向换成长边:
 *   沿 u 扫 24 段, 每段沿短边取 9 点, 从两端往里找"连续 3 段覆盖率都 >= 4/9"的位置当边界。
 * 实测效果: 上面那 12 帧收完长边比例全部落到 3.20~3.36 —— 正好是真车牌的比例。
 * 守卫(与短边同一套路, 宁可不收也不能切字): 单端最多收 ROI_UTRIM_MAX, 两端收完至少留 ROI_UTRIM_MIN_KEEP。
 * apply=false => 只测不打(纯诊断); apply=true => 把区间写回 out_u_lo/out_u_hi 供取样用。
 */''',
''' * 同一套"收边"逻辑短边方向早就在用 (roi_trim_short_axis), 这里照它的骨架把方向换成长边:
 *   沿 u 扫 24 段, 每段沿短边取 9 点, 从两端往里找"连续 3 段覆盖率都 >= 4/9"的位置当边界。
 * 实测效果: 上面那 12 帧收完长边比例全部落到 3.20~3.36 —— 正好是真车牌的比例。
 * P5.59 定案: **只收左端**(判据见 ROI_UTRIM_LEFT_MAX 上面那张实测表), 右端一律保持 1.0 留白 ——
 *   右端一收就会把末尾字切出取样窗 (#642/#645 各多认一个 6)。
 * apply=false => 只测不动(纯诊断, 轴对齐回退路径用); apply=true => 写回 out_u_lo/out_u_hi 供取样用。
 */''',
"E4 函数说明")

# ---------------- E5: 函数体 ----------------
P(
'''    // P5.37 扳机: 框没偏宽就别动它。框"偏高"(比例小)时左右本来就没多, 硬收只会让形状更离谱 ——
    //   实测豫FSQ818 的框是 2.4~2.5, 收边开着 3/5 对、关着 8/8 全对。
    if (ratio_now <= ROI_UTRIM_TRIGGER) {
        if (verbose) ESP_LOGI(TAG, "长边收边: 框现在比例 %.2f 没偏宽 (> %.2f 才收) -> 不动它",
                              ratio_now, ROI_UTRIM_TRIGGER);
        return;
    }
    float u_lo = (float)jl / (float)N;
    float u_hi = (float)(jr + 1) / (float)N;
    if (u_lo > ROI_UTRIM_MAX) u_lo = 0.0f;             // 单端收太多 => 这一端不收
    if (1.0f - u_hi > ROI_UTRIM_MAX) u_hi = 1.0f;
    if (u_hi - u_lo < ROI_UTRIM_MIN_KEEP) {            // 两端合起来剩太少 => 整块不收
        if (verbose) ESP_LOGW(TAG, "长边收边: 收完只剩 %.0f%% 长边, 太狠 -> 整块不收", (u_hi - u_lo) * 100.0f);
        return;
    }
    // P5.37 下限: 收完比真车牌还窄 => 边界找错了(多半是把泛蓝的牌面带切了), 整块不收
    const float ratio_new = (vlen > 1.0f) ? (ulen * (u_hi - u_lo) / vlen) : 0.0f;
    if (ratio_new < ROI_UTRIM_FLOOR) {
        if (verbose) ESP_LOGW(TAG, "长边收边: 收完比例 %.2f 比真车牌还窄 -> 整块不收 (长边 %.0fpx 短边 %.0fpx)",
                              ratio_new, ulen, vlen);
        return;
    }
    if (apply) {
        if (out_u_lo) *out_u_lo = u_lo;
        if (out_u_hi) *out_u_hi = u_hi;
    }
    if (verbose) {
        ESP_LOGI(TAG, "长边收边%s: 长边 %.0fpx 短边 %.0fpx 现在比例 %.2f -> %s 左 %.0fpx 右 %.0fpx, 取样长边 %.0fpx 比例 %.2f (真车牌 3.1~3.4)",
                 apply ? "(已启用)" : "(未启用,只测不动)", ulen, vlen, ratio_now,
                 apply ? "收掉" : "可收", u_lo * ulen, (1.0f - u_hi) * ulen, ulen * (u_hi - u_lo), ratio_new);
    }
}''',
'''    // P5.59: 只收左端 —— 右端一律保持 1.0 (留白), 理由见 ROI_UTRIM_LEFT_MAX 上面那张实测表。
    const float c_lo = (float)jl / (float)N;
    const float c_hi = (float)(jr + 1) / (float)N;
    if ((c_hi - c_lo) < ROI_UTRIM_BAND_MIN) {          // 蓝带太窄 => 边界不可信
        if (verbose) ESP_LOGW(TAG, "长边收边: 蓝带只占 %.0f%% 长边 (< %.0f%%) -> 边界不可信, 这一帧不收",
                              (c_hi - c_lo) * 100.0f, ROI_UTRIM_BAND_MIN * 100.0f);
        return;
    }
    if (c_lo > ROI_UTRIM_LEFT_MAX) {                   // 左端要收掉太多 => 多半量错了
        if (verbose) ESP_LOGW(TAG, "长边收边: 左端要收掉 %.0f%% (> %.0f%%) -> 多半量错了, 这一帧不收",
                              c_lo * 100.0f, ROI_UTRIM_LEFT_MAX * 100.0f);
        return;
    }
    if (apply) {
        if (out_u_lo) *out_u_lo = c_lo;   // 只动左端
        if (out_u_hi) *out_u_hi = 1.0f;   // 右端留白 (防末尾字被切出窗)
    }
    if (verbose) {
        const float ratio_new = (vlen > 1.0f) ? (ulen * (1.0f - c_lo) / vlen) : 0.0f;
        ESP_LOGI(TAG, "长边收边%s(只收左): 长边 %.0fpx 短边 %.0fpx 比例 %.2f -> %s 左端 %.0fpx = %.0f%%, 右端不动; 取样长边 %.0fpx 比例 %.2f (真车牌 3.1~3.4)",
                 apply ? "(已启用)" : "(未启用,只测不动)", ulen, vlen, ratio_now,
                 apply ? "收掉" : "可收", c_lo * ulen, c_lo * 100.0f, ulen * (1.0f - c_lo), ratio_new);
    }
}''',
"E5 函数体")

# ---------------- E6: 删影子裁块 ----------------
P(
'''            // ==== P5.57 影子对照: 再按"实测蓝带"裁两块, 待会儿各跑一次模型 —— 只打日志, 不改本帧报告 ====
            //   要回答的问题: 把取样范围收到蓝牌真正的两端上, 能不能治好"省字错"(京 -> 皖)?
            //   B = 两端都收到蓝带;  C = 只收左端(右端留白 —— 历史上右端留白是防末尾字被切出窗)。
            //   \u26a0 必须在这里裁: 下面那段会把 box.qx/qy 收缩成"取样段", 之后就还原不出原始四角了。
            {
                g_shadow_n = 0;
                const bool band_ok = (cov_lo >= 0.0f) && (cov_hi > cov_lo) && ((cov_hi - cov_lo) >= 0.60f);
                if (band_ok && cov_lo <= 0.45f) {
                    const float bu[2][2] = {{cov_lo, cov_hi}, {cov_lo, 1.0f}};
                    for (int v = 0; v < 2; v++) {
                        if ((1.0f - bu[v][1]) > 0.45f) continue;   // 右端要收掉四成以上 => 多半是量错了, 这个变体跳过
                        int dx = 0, dy = 0;
                        float dn = 1.0f;
                        sample_quad_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                             bu[v][0], bu[v][1], v_lo, v_hi,
                                             g_shadow_i8[g_shadow_n], norm_lut, &dx, &dy, &dn);
                        g_shadow_n++;
                    }
                }
            }
''',
'''            // P5.59: P5.57 的影子对照已按定论删除(定论 = 只收左端), 这里不再额外裁块。
''',
"E6 影子裁块")

# ---------------- E7: 删影子打印 ----------------
P(
'''        // P5.57 影子对照 —— 把上面裁好的影子块各跑一次模型。
        //   报告结果仍然是 plate(现状), 这一行只用来回答"哪种收边更好"。
        //   判读: 现状错、影子对  => 那个变体就是解药;  三者一致 => 这一帧对收边不敏感(不构成证据)。
        if (g_shadow_n > 0) {
            std::string sh_a = "(未裁)", sh_b = "(未裁)";
            for (int v = 0; v < g_shadow_n; v++) {
                const std::string sr = shadow_run(model, model_input, output_float, g_shadow_i8[v]);
                if (v == 0) sh_a = sr; else sh_b = sr;
            }
            char band[72] = "";
            if (cov_lo >= 0.0f && cov_hi > cov_lo)
                snprintf(band, sizeof(band), " 蓝带[%.0f%%~%.0f%%] 多吃[左%.0f%% 右%.0f%%]",
                         cov_lo * 100.0f, cov_hi * 100.0f, cov_lo * 100.0f, (1.0f - cov_hi) * 100.0f);
            ESP_LOGW(TAG, "#%d 影子: 现状=%s | 两端收=%s | 只收左=%s%s%s",
                     fn, plate.c_str(), sh_a.c_str(), sh_b.c_str(), band,
                     (plate == sh_a && plate == sh_b) ? " [三者一致]" : " [有分歧]");
        }

''',
'',
"E7 影子打印")

# ---------------- E8: 开关注释 ----------------
P(
'''// P5.34: "长边收边"开关。开 = 取样框按实测蓝色边界把两端收窄 (修 CTC 在两端多吐字); 关 = 回到 P5.33 行为。
//   BOOT 双击切换 (P5.35 起; 原先是 1~2s 中按, 用户反馈太容易误触)。默认开 —— 实测 12 帧里 10 帧错、且错法一致, 收完比例全部落到真车牌区间。''',
'''// P5.34/P5.59: "长边收边"开关。开 = 取样框**左端**按实测蓝色边界收进来 (修 CTC 在左边多吐一个省字); 关 = 回到 P5.33 行为。
//   BOOT 双击切换 (P5.35 起; 原先是 1~2s 中按, 用户反馈太容易误触)。默认开 —— 34 帧影子对照: 省字对 59% -> 76%。''',
"E8 开关注释")

# ---------------- E9: 按键日志 ----------------
P(
'''                    ESP_LOGW(TAG, "长边收边: %s —— %s", g_utrim_on ? "开" : "关",
                             g_utrim_on ? "取样框按实测蓝色边界收窄, 少认两端多出来的字"
                                        : "取样框维持定位框原样 (回到 P5.33 行为)");''',
'''                    ESP_LOGW(TAG, "左端收边(P5.59): %s —— %s", g_utrim_on ? "开" : "关",
                             g_utrim_on ? "取样框左端按实测蓝色边界收进来, 少认左边多出来的省字"
                                        : "取样框维持定位框原样 (回到 P5.33 行为)");''',
"E9 按键日志")

P(
'''                                   g_utrim_on ? "BOOT 双击(收边开)" : "BOOT 双击(收边关)", true);''',
'''                                   g_utrim_on ? "BOOT 双击(收左开)" : "BOOT 双击(收左关)", true);''',
"E9b 触发串")

# ---------------- E10: 帮助横幅 ----------------
P(
'''    ESP_LOGI(TAG, "    P5.37: 收边加了扳机 —— 只在[框明显偏宽 >3.4]时才收; 框偏高(如 2.4~2.5 的牌)一律不动, 免得帮倒忙");''',
'''    ESP_LOGI(TAG, "    P5.37: 收边加过扳机(只在[框明显偏宽 >3.4]时才收) —— **P5.59 已废**: 实测省字错来自左端多吃背景, 与框宽高比无关");''',
"E10 横幅P5.37")

P(
'''    ESP_LOGI(TAG, "    P5.57: **拆掉每帧诊断 + 加影子对照** —— 原先每帧额外跑 2 次推理(重放/探针)且强制发一张 94x24 图, 约 +800ms/帧; 现在改成每帧多裁 2 块(蓝带两端收 / 只收左端)各跑一次, 只打一行 `影子:` 日志, 用来判定哪种长边收边更好");''',
'''    ESP_LOGI(TAG, "    P5.57(已在 P5.59 删掉): **拆掉每帧诊断 + 影子对照** —— 曾每帧多裁 2 块(蓝带两端收 / 只收左端)各跑一次模型, 只打一行 `影子:` 日志; 结论拿到后连影子一起去掉, 换回速度");
    ESP_LOGI(TAG, "    P5.59: **长边收边定案 = 只收左端(右端留白)** —— 34 帧 京Q06666 影子对照: 现状 整串47%%/省字59%%, 两端收 47%%/76%%, 只收左 50%%/76%%; 两端收有 2 帧(#642/#645)把末尾字切掉 -> 淘汰。同时废掉 P5.37 的扳机/下限");''',
"E11 横幅P5.57/59")

P(
'''    ESP_LOGI(TAG, "    BOOT: 单击(<1s)=切图片模式 | 双击=长边收边开关 | 三击=循环切取样几何档 | 长按(>=2s)=详细模式开关");''',
'''    ESP_LOGI(TAG, "    BOOT: 单击(<1s)=切图片模式 | 双击=左端收边开关 | 三击=循环切取样几何档 | 长按(>=2s)=详细模式开关");''',
"E12 横幅BOOT")

# ---------------- E13: 版本号 ----------------
P(
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.58 ===");''',
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.59 ===");''',
"E13 版本横幅")

s = io.open(SRC, encoding="utf-8", newline="").read()
bad = []
for old, new, tag in PATCHES:
    n = s.count(old)
    print("%-16s 命中 %d %s" % (tag, n, "OK" if n == 1 else "*** 不是 1 ***"))
    if n != 1: bad.append(tag)
if bad:
    print("\n锚点不唯一, 未写入: %s" % bad); raise SystemExit(1)
n0 = len(s)
for old, new, tag in PATCHES: s = s.replace(old, new, 1)
io.open(SRC, "w", encoding="utf-8", newline="").write(s)
print("\n全部生效, %d -> %d" % (n0, len(s)))
for dead in ("g_shadow", "shadow_run", "ROI_UTRIM_MAX", "ROI_UTRIM_MIN_KEEP", "ROI_UTRIM_TRIGGER", "ROI_UTRIM_FLOOR"):
    assert dead not in s, "还有残留: " + dead
print("影子/旧扳机 残留检查通过")
