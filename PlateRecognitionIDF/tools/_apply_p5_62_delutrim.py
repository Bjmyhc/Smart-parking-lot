# -*- coding: utf-8 -*-
"""_apply_p5_62_delutrim.py -- 把"长边收边"整段删除 (P5.61 追加改动).

自底向上按行号范围改, 每处都校验首行内容, 任一处对不上就整体放弃。
"""
import io, sys
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
src = io.open(P, encoding="utf-8", newline="").read()
lines = src.splitlines(True)

def L(n):            # 1-based
    return lines[n - 1]

OPS = []   # (start, end, new_lines(list[str] or None), expect_substr)

OPS.append((115, 115, ["#define BOOT_DBLCLICK_MS 400 // P5.35: 单击后等这么久, 期间又来一下 = 双击 (P5.61 起 = 复位图片输出)"], "#define BOOT_DBLCLICK_MS"))

OPS.append((593, 593, ["//   (那原本是\"长边收边\"该管的 —— 但 P5.61 复算证明收边在切省字, 已整段删除, 见文档第四十五节。三击仍可现场切回任何一档。)"], "那正是"))

OPS.append((600, 600, ["//   171 帧原始帧复算(长边收边已删): 左 +1% 128/171 -> 左 +3% **138/171 = 81%** -> 左 +5% 117/171;"], "171 帧原始帧复算"))

OPS.append((777, 786, [
 "// P5.34/P5.59: \"长边收边\"(按实测蓝色边界把取样框左端收窄) —— **P5.61 已整段删除**。",
 "//   定论: 171 张原始帧按固件真流程复算, 收左端 76/171 = 44%, 不收 128/171 = 75%,",
 "//   而且\"收左端对\"的 76 帧完全落在\"不收对\"的 128 帧里面 —— 0 帧有帮助、52 帧帮倒忙。",
 "//   根因: P5.60 换了 b-r 判据之后定位框本来就贴着车牌, 蓝带量出来的\"左端那一段不是蓝\"其实就是",
 "//   **省字自己**(省字笔画太密, 一行 9 个采样点里蓝点常常不到 4 个), 再按它收 = 把省字切掉,",
 "//   真机表现就是 京 -> 皖/粤/沪 乱跳、或者整串少一个字。详见文档第四十五节。",
], "P5.34/P5.59"))

OPS.append((1887, 1898, [
 "// P5.61: 这里原来还有 `ROI_UTRIM_LEFT_MAX` / `ROI_UTRIM_BAND_MIN` 两个常数, 配合同期的 roi_probe_utrim()",
 "//   做\"长边收边\"(只收左端)。**已整段删除** —— 复算证明它 0 帧有帮助、52 帧帮倒忙(切省字),",
 "//   根因见文档第四十五节。下面保留的只有\"短边收边\"(P5.9 起的 ROI_VTRIM_*), 那个是有效的。",
], "P5.59: 长边收边"))

OPS.append((2041, 2041, ["                                 float u_lo, float u_hi,      // P5.61: 固定 [0,1] (长边收边已删, 形参保留)"], "float u_lo, float u_hi,"))

OPS.append((2047, 2047, ["    // P5.61: u 恒为 [0,1] (长边收边已删), 所以 span_u_raw 恒为 1。"], "留白改成"))

OPS.append((2206, 2299, [
 "// P5.61: 这里原来是 roi_probe_utrim() —— 沿长边(u)方向量\"蓝色带\"并把取样框左端收窄。",
 "//   **整个函数已删除**: 复算证明它把省字切掉(收左端 44% vs 不收 75%, 且 0 帧有帮助)。",
 "//   现在取样框就是定位框原样 + g_pad_u_l/g_pad_u_r 留白。详见文档第四十五节。",
], "/**"))

OPS.append((2385, 2389, [
 "        float v_lo = 0.0f, v_hi = 1.0f;   // P5.9: 短边方向的取样区间 (默认不收)",
 "        float norm_s = 1.0f;              // P5.10: 裁剪块光度归一化的倍数 (1.0 = 没动)",
], "float v_lo = 0.0f"))

OPS.append((2393, 2395, None, "再实测\"蓝色区域的左右边界\""))

OPS.append((2404, 2406, [
 "            sample_quad_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,",
 "                                 0.0f, 1.0f, v_lo, v_hi,",
 "                                 (int8_t *)model_input->data, norm_lut, &ss_nx, &ss_ny, &norm_s);",
], "sample_quad_to_input"))

OPS.append((2407, 2415, None, "P5.59: P5.57 的影子对照"))

OPS.append((2418, 2425, None, "回退路径同样量一次长边覆盖率"))

OPS.append((2520, 2529, [
 "            // P5.61: 这里原来还打 [蓝带[左%~右%] 多吃[左%,右%]] —— 它是\"长边收边\"的量尺,",
 "            //   收边删掉之后这个字段没有任何消费者, 一并去掉 (顺便省掉每帧 24x9 次采样)。",
], "P5.56: 字段改名"))

OPS.append((2533, 2533, ["                     v_lo * 100.0f, v_hi * 100.0f,"], "v_lo * 100.0f, v_hi * 100.0f, covs,"))

OPS.append((2611, 2611, ["            //   已经打过完全相同的一份 (同样的框), 纯属重复。"], "同样的框、同样的收边区间"))

OPS.append((2780, 2780, ["    ESP_LOGI(TAG, \"    P5.61: **长边收边整段删除 + 左留白 净0%%->净+2%% + 上下留白 净5%%->净11%%** —— 171 帧原始帧按固件真流程复算: 整串全对 44%% -> 84%%, 省字对 91%%\");"], "P5.61: **长边收边改成默认关"))

OPS.append((2784, 2785, ["    ESP_LOGI(TAG, \"    P5.36~P5.59: 长边收边(按实测蓝色边界把取样框左端收窄) —— **P5.61 已整段删除**(0 帧有帮助 / 52 帧帮倒忙)\");"], "P5.36: 长边收边正式启用"))

OPS.append((2789, 2790, None, "ROI 行里的 [蓝带"))

OPS.append((2803, 2803, ["    ESP_LOGI(TAG, \"    P5.59(P5.61 已整段删除): 长边收边曾定案\\\"只收左端\\\" —— 复算证明它 0 帧有帮助/52 帧帮倒忙, 代码已删; 详见文档第四十五节\");"], "P5.59(已在 P5.61 废弃)"))

OPS.append((2807, 2807, ["    ESP_LOGI(TAG, \"    BOOT: 单击(<1s)=切图片模式 | 双击=复位图片输出 | 三击=循环切取样几何档 | 长按(>=2s)=详细模式开关\");"], "BOOT: 单击(<1s)=切图片模式"))

OPS.append((2882, 2883, [
 "            ESP_LOGI(TAG, \"说明: 周期 %u ms => 背靠背连续识别, 精简模式; BOOT 单击(<1s)=切图片输出(当前 %d), 双击=复位图片输出, 三击=取样几何档%d(左净 %+.0f%% 右净 %+.0f%%), 长按(>=2s)=详细模式开关\",",
 "                     (unsigned)AUTO_PERIOD_MS, g_img_mode, g_pad_profile,",
 "                     (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);",
], "周期 %u ms => 背靠背连续识别"))

OPS.append((2886, 2886, ["            ESP_LOGI(TAG, \"触发方式: 开机 1 次 + 单击(<1s)BOOT 切图片模式并拍一帧 + 双击复位图片输出 + 三击循环切取样几何档 + 长按(>=2s)切详细模式开关\");"], "双击切长边收边"))

OPS.append((2902, 2902, ["                    // P5.35/P5.61: 双击 = 复位图片输出 (原来是 1~2s 中按, 太容易误触 —— 用户实测反馈)。"], "双击 = 长边收边开关"))

OPS.append((2927, 2927, ["                    // P5.35: 单击要等一下再执行 —— 看 BOOT_DBLCLICK_MS 内还有没有第二下, 有就是双击(=复位图片输出), 没有才是单击。"], "双击(=长边收边开关)"))

OPS.append((2961, 2971, [
 "                } else if (dbl_click) {",
 "                    // P5.61: 双击原来 = \"长边收边\"开关, 收边已整段删除; 现在改成 **图片输出全复位 + 打一张发图统计** ——",
 "                    //   串口图卡住时双击一下就能救回来, 而且不改图片模式 (不会因为救卡而多刷几帧)。",
 "                    // ⚠ 这一支必须排在 short_press 前面: dbl_click 为真时 short_press 必然也为真,",
 "                    //   放在后面编译器会判定\"永远走不到\"而整块删掉 (第一次写反就是这么踩的)。",
 "                    img_tx_reset(\"BOOT 双击\");",
 "                    ESP_LOGW(TAG, \"发图统计: 累计成功 %u 帧 / %u KB; 连续失败 %u 次 (最后原因: %s)\",",
 "                             (unsigned)g_tx_ok, (unsigned)(g_tx_bytes / 1024), (unsigned)g_tx_fail,",
 "                             g_tx_fail_why ? g_tx_fail_why : \"-\");",
 "                    recognize_once(model, model_input, output_float, norm_lut, \"BOOT 双击(图片输出复位)\", true);",
], "} else if (dbl_click) {"))

OPS.append((2996, 2996, ["                    ESP_LOGW(TAG, \"按键 %d ms 落在 1~2s 空档里, 当作没按 (单击<1s=切图片模式, 双击=复位图片输出, 三击=循环切取样几何档, 长按>=2s=详细模式开关)\", (int)dur_ms);"], "落在 1~2s 空档里"))

bad = 0
for (a, b, new, exp) in OPS:
    if exp not in L(a):
        print("MISMATCH at line %d: expect %r, got %r" % (a, exp, L(a)[:80]))
        bad += 1
    if b > len(lines):
        print("RANGE OUT OF FILE at %d-%d (total %d)" % (a, b, len(lines)))
        bad += 1
print("checked=%d bad=%d total_lines=%d" % (len(OPS), bad, len(lines)))
if bad:
    sys.exit(1)

for (a, b, new, exp) in sorted(OPS, key=lambda x: -x[0]):
    rep = [] if new is None else [s + "\n" for s in new]
    lines[a - 1:b] = rep

io.open(P, "w", encoding="utf-8", newline="").write("".join(lines))
print("OK, wrote %d lines" % len(lines))
