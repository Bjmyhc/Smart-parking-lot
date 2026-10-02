# -*- coding: utf-8 -*-
"""P5.57:
   拆: P5.51~P5.55 的**每帧**诊断 (重放/探针 2 次多余推理 + 每帧强制发裁块图 + 输入校验和打印)
   加: 「长边收边」影子对照 —— 每帧额外裁两块 (B 两端收到蓝带 / C 只收左端) 各跑一次模型, 只打日志
"""
import io

SRC = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
PATCHES = []
def P(old, new, tag): PATCHES.append((old, new, tag))

# ---------------- 1) 全局缓冲: 换掉 g_live_in / g_live_f ----------------
P(
'''// P5.53: 实时那一帧喂进模型的原始 int8 字节 (run() 之前留档), 以及重放用的 float 版本
static int8_t g_live_in[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_live_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));''',
'''// P5.57 影子对照: 两个"长边收边"变体各裁一份 int8 模型输入, 以及跑模型用的 float 中转。
//   每帧复用, 不参与任何报告状态 —— 报告结果永远是"现状"那条链路算出来的。
static int8_t g_shadow_i8[2][IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static float  g_shadow_f[IMG_W * IMG_H * 3] __attribute__((aligned(16)));
static int    g_shadow_n = 0;''', "影子缓冲声明")

# ---------------- 2) shadow_run 助手 ----------------
P(
'''static bool recognize_once(dl::Model *model,''',
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

static bool recognize_once(dl::Model *model,''', "shadow_run 助手")

# ---------------- 3) 影子裁块: 紧跟在现状那次 sample_quad_to_input 之后 ----------------
P(
'''            sample_quad_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                 u_lo, u_hi, v_lo, v_hi,
                                 (int8_t *)model_input->data, norm_lut, &ss_nx, &ss_ny, &norm_s);
            if (u_lo > 0.0f || u_hi < 1.0f) {''',
'''            sample_quad_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                 u_lo, u_hi, v_lo, v_hi,
                                 (int8_t *)model_input->data, norm_lut, &ss_nx, &ss_ny, &norm_s);
            // ==== P5.57 影子对照: 再按"实测蓝带"裁两块, 待会儿各跑一次模型 —— 只打日志, 不改本帧报告 ====
            //   要回答的问题: 把取样范围收到蓝牌真正的两端上, 能不能治好"省字错"(京 -> 皖)?
            //   B = 两端都收到蓝带;  C = 只收左端(右端留白 —— 历史上右端留白是防末尾字被切出窗)。
            //   ⚠ 必须在这里裁: 下面那段会把 box.qx/qy 收缩成"取样段", 之后就还原不出原始四角了。
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
            if (u_lo > 0.0f || u_hi < 1.0f) {''', "影子裁块")

# ---------------- 4) 拆掉 P5.54 的校验和 ----------------
P(
'''        // P5.54: 裁块点的缓冲校验和 —— 和 run() 前那一次比对, 就能知道中间有没有人动过输入
        const uint32_t ck_crop = mem_fnv32(model_input->data, (size_t)IMG_W * IMG_H * 3);

''', '''''', "拆 P5.54 校验和")

# ---------------- 5) 拆掉 P5.53 的留档 + 打印 ----------------
P(
'''        // P5.53: 原样留一份"实时这一帧真正喂进去的字节" (run() 之后这块显存会被 esp-dl 挪作他用)
        memcpy(g_live_in, model_input->data, sizeof(g_live_in));
        const uint32_t ck_pre = mem_fnv32(g_live_in, sizeof(g_live_in));
        ESP_LOGI(TAG, "#%d 输入缓冲: 裁块点=%08X run前=%08X %s", fn, (unsigned)ck_crop, (unsigned)ck_pre,
                 (ck_crop == ck_pre) ? "一致" : "*** 中间被改过 ***");

''', '''''', "拆 P5.53 留档")

# ---------------- 6) 拆掉 P5.52/P5.55 的重放+探针, 换成影子结果打印 ----------------
P(
'''        // P5.52: 同帧对照 —— 实时结果是坏的时, 立刻用"已知能读对的探针"再跑一次同一条链路。
        //   实时坏 + 探针也坏  => 环境/内存问题 (和数据无关)
        //   实时坏 + 探针还好  => 这一帧喂进去的数据确实和发出来的裁块不一样
        if (true) {   // P5.55 临时: 诊断期每帧都跑对照
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
        }''',
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
        }''', "影子结果打印")

# ---------------- 7) 拆掉每帧强制发裁块图 ----------------
P(
'''    // P5.55 临时诊断: 每帧都编一张 94x24 的 JPEG 发出去 —— 专门验"发图这条路"会不会搅坏模型
    img_tx_send_crop();

''', '''''', "拆每帧强制发图")

# ---------------- 8) 版本横幅 ----------------
P(
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.56 ===");''',
'''ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.57 ===");''', "版本横幅")

P(
'''    ESP_LOGI(TAG, "    P5.56: **兜底档加 %.1f%% 占屏下限**''',
'''    ESP_LOGI(TAG, "    P5.57: **拆掉每帧诊断 + 加影子对照** —— 原先每帧额外跑 2 次推理(重放/探针)且强制发一张 94x24 图, 约 +800ms/帧; 现在改成每帧多裁 2 块(蓝带两端收 / 只收左端)各跑一次, 只打一行 `影子:` 日志, 用来判定哪种长边收边更好");
    ESP_LOGI(TAG, "    P5.56: **兜底档加 %.1f%% 占屏下限**''', "帮助文本 P5.57")

s = io.open(SRC, encoding="utf-8", newline="").read()
bad = []
for old, new, tag in PATCHES:
    n = s.count(old)
    print("%-20s 命中 %d %s" % (tag, n, "OK" if n == 1 else "*** 不是 1 ***"))
    if n != 1: bad.append(tag)
if bad:
    print("\n锚点不唯一, 未写入: %s" % bad); raise SystemExit(1)
n0 = len(s)
for old, new, tag in PATCHES: s = s.replace(old, new, 1)
io.open(SRC, "w", encoding="utf-8", newline="").write(s)
print("\n全部生效, 长度 %d -> %d" % (n0, len(s)))
