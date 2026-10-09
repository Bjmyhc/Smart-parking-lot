/*
 * 车牌识别 - ESP32-S3 端侧推理 (P5: 接摄像头 -> 串口)
 *
 * 数据流: esp32-camera 取 RGB565 帧
 *         -> dl::image::resize 一次完成「ROI 裁剪 + 双线性缩放到 94x24 + 通道序 + 量化」
 *         -> LPRNet 推理 -> CTC 贪婪解码 -> 串口打印
 *
 * ⚠ 张量布局 (踩过的坑, 别再改回去)
 *   esp-dl 内部张量一律是 NHWC —— 由 vision/image/dl_image_preprocessor.cpp:15-18 可确认:
 *   shape[1]=H, shape[2]=W, shape[3]=C, 数据按像素交织存放。
 *   本模型输入 [1,24,94,3], 输出 [1,68,18] (68=NUM_CLASS, 18=TIME_STEPS;
 *   行主序 => logits[c*18+t], 与旧版 main.cpp 的 greedy_decode 一致)。
 *
 * ⚠ 预处理必须与训练一致 (tools/quant_espdl_s3.py:41-52 == predict.py)
 *   训练链路: cv2.imdecode(BGR) -> cv2.resize(94x24, INTER_LINEAR) -> (x-127.5)*0.0078125
 *   模型输入 exponent=-7 => scale=2^-7, 即 mean=127.5 / std=128。
 *   板端把归一化压成一张 768 字节 (3x256) 的 LUT, 交给 resize 在缩放时逐像素查表:
 *       lut[c*256+v] = clamp(round((v-127.5)/128 * 128)), 其中 round 用 esp-dl 在 S3 上的取整
 *   通道序必须是 BGR: caps 带 DL_IMAGE_CAP_RGB_SWAP
 *   (dl_image_color.hpp 的 rgb565->rgb888_quant 在 RGB_SWAP 分支写 dst[2]=R,dst[1]=G,dst[0]=B
 *    即 BGR —— 与 cv2 的 BGR 一致; 且缩放后那一步量化 caps 传 0 不再交换, 顺序被保留)。
 *
 * ⚠ RGB565 字节序 (最容易搞反的一处)
 *   esp32-camera 的 RGB565 帧按大端存放: conversions/to_jpg.cpp:34 的 rgb565_big_endian=true,
 *   其大端分支 R=src[i]&0xF8 / G=((src[i]&7)<<5)|((src[i+1]&0xE0)>>3) / B=(src[i+1]&0x1F)<<3,
 *   与 esp-dl dl_image_define.hpp:17-19 的 DL_IMAGE_BIG_ENDIAN_RGB565_* 宏逐位一致
 *   (src 的 uint16 视图是 CPU 小端读出的)。
 *   => caps 必须带 DL_IMAGE_CAP_RGB565_BIG_ENDIAN。
 *      该 flag 的含义是「按大端宏解析」, 不是「数据是小端」, 名字反直觉, 一律以宏定义为准。
 */
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dl_define.hpp"
#include "dl_image_define.hpp"
#include "dl_image_process.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"   // P5.12: 串口图片预览用的软 JPEG 编码器
#include "esp_rom_uart.h"   // P5.12: esp_rom_output_tx_one_char —— 原样发字节, 不做换行转换
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "node_link.h"   // P5.68: 摄像头 -> 节点单片机 链路 (P5.80 起走 UART1: TX=GPIO47 / RX=GPIO48)

static const char *TAG = "plate";

/*
 * P5.76: 不带 "I (123) plate:" 前缀的直出 —— 结果块和开机那几行走这里, 和 $PLATE 一样干净。
 *   直接写 stdout (和控制台同一个口), 末尾自己补 '\n' (控制台 VFS 会转成 CRLF, 别再写 '\r')。
 */
static void plain_out(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(buf) - 2) n = (int)(sizeof(buf) - 2);
    buf[n++] = '\n';
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
}

/* P5.76: 一次识别的"结果块" —— 简单模式(默认)下这就是全部日志 */
static void print_plate_block(const char *plate, float conf_pct, int frame, int x1, int y1, int x2, int y2)
{
    plain_out("========================================");
    plain_out("车牌号码: %s", plate);
    plain_out("车牌颜色: blue");
    plain_out("置信度:   %.2f%%", conf_pct);
    plain_out("帧号:     %d", frame);
    plain_out("车牌位置: (%d,%d) -> (%d,%d)", x1, y1, x2, y2);
    plain_out("========================================");
}

/* P5.77: 一次识别的"未识别块" —— 被点名拍的那一帧(AT+RUN)没出结果时必打。
 *   简单模式下一个字都没有的话, 根本分不清"没定位到 / 模型没认出 / 取帧失败";
 *   连续模式(每 200ms 一帧)不打, 否则刷屏。 */
static void print_fail_block(const char *reason, int frame)
{
    plain_out("========================================");
    plain_out("车牌号码: - (未识别)");
    plain_out("原因:     %s", (reason && *reason) ? reason : "(未知原因)");
    if (frame > 0) plain_out("帧号:     %d", frame);
    else           plain_out("帧号:     -");
    plain_out("========================================");
}

// 由 CMake target_add_aligned_binary_data 嵌入到 flash rodata
extern const uint8_t model_espdl[] asm("_binary_lprnet_s3_espdl_start");
extern const uint8_t test_input_bin[] asm("_binary_test_input_bin_start");
// P5.50 自检探针: 用户实拍那一帧的 94x24 模型输入块 (板端自己发回来的), PC 端 float 读作 京Q06666
extern const uint8_t probe_crop_bin[] asm("_binary_probe_crop_bin_start");
// P5.66: 省字复核模型 (只看第一个字) —— 直接从原图的四边形抠省字格, 不复用 94x24 那条把省字压成 13x22 的路
extern const uint8_t prov_espdl[] asm("_binary_prov_s3_espdl_start");

static const int IMG_H = 24;
static const int IMG_W = 94;
static const int NUM_CLASS = 68;   // 65 字符 + blank, blank = 67
static const int TIME_STEPS = 18;

// ===================== P5.66: 省字复核模型 (第二个 espdl) =====================
// 为什么要它: 主模型吃的是 94x24, 省字那一格只剩 13x22 = 286 像素; 而原图 (640x480) 里
// 同一个省字有 44x138 = 6072 像素 —— 21 倍的信息是**缩采样那一步扔掉的**, 事后锐化补不回来。
// 实测证据: 同一张 94x24 裁块, 人眼读「京」, 主模型 93~99% 自信地读「皖」(replay 集 95% 是皖牌)。
static const int PROV_W = 32;                   // 输入宽
static const int PROV_H = 64;                   // 输入高
static const int PROV_CLASS = 31;               // 31 个省字
static const float PROV_U_SPAN = 1.0f / 7.35f;  // 省字在车牌长边方向占的区间 (u 方向)
static const float PROV_NORM_MEAN = 127.5f;     // 训练里的 (x/255 - 0.5)/0.25
static const float PROV_NORM_STD = 63.75f;
// 置信门槛: PC 端 4 折交叉验证里, 只采纳 >=0.70 的那些, 准确率 ~98%; 不过门槛就保留主模型首字
static const float PROV_CONF_MIN = 0.70f;
// ===================== P5.67: 省字窗口的「蓝面锚点 + 多候选取最优」 =====================
// 根因 (PC 归因实验 tools/p571_prov_robust.py + tools/p572_prov_domain.py, 245 张真实省字 patch):
//   提亮/泛白/模糊/低分辨率/反光斑/JPEG 压缩/色偏 都打不垮它 (置信中位仍有 76~96%),
//   唯独"窗口横向偏 20%/40% 个窗宽"能把它从中位 94% 打到 82%/39% (粤 top-1 只剩 9%);
//   而窗口的**宽窄**(拉伸/压缩)完全不影响 => 要修的是窗口**左边界的位置**, 不是宽度。
// 板上的对应现象: 同一块牌相邻帧 95% -> 22% -> 95% 乱跳; 且低置信集中在兜底框
//   (兜底帧 27% 过不了 70% 门槛, 严格框只有 5%) —— 兜底框左边缘松, 而省字窗的左边界原来就是框的左角。
// 修法: 左边界不再取框的 u=0, 改为沿车牌中线扫出"蓝面真正的左边缘"当锚点, 再在锚点两侧各试 K 扇窗。
static const int   PROV_SCAN_N    = 160;      // 锚点扫描: 中线上一共采多少点 (只扫 u ∈ [0, PROV_SCAN_UMax])
static const float PROV_SCAN_UMax = 0.5f;     // 省字一定在左半边, 扫前半段就够
static const float PROV_CAND_STEP = 0.0068f;  // 相邻候选窗的 u 间距 ≈ 窗宽(13.6%)的 5%
static const int   PROV_CAND_K    = 2;        // 锚点两侧各试 K 扇 => 搜索半径 ≈ 窗宽的 ±10%
static const int   PROV_CAND_MAX  = 2 * 2 + 2;  // 候选上限 (含"老做法 u=0"那一扇)
#define PROV_DUMP_ZOOM        4               // 发回串口时把 patch 放大几倍
#define PROV_DUMP_MIN_GAP_US  10000000LL      // 连续模式下"复核没过门槛就发 patch"的限流: 10s 一次
// ⚠ 类别顺序必须与 tools/p565f_train2.py 的 PROV 一致 —— 与主模型 CHARS 的顺序**不同**, 别照抄
static const char *PROV_CHARS[PROV_CLASS] = {
    "京", "津", "冀", "晋", "蒙", "辽", "吉", "黑", "沪", "苏", "浙", "皖", "闽", "赣", "鲁", "豫",
    "鄂", "湘", "粤", "桂", "琼", "渝", "川", "贵", "云", "藏", "陕", "甘", "青", "宁", "新"};
static uint32_t mem_fnv32(const void *p, size_t n);   // 定义在后面, 先用先声明

// 自动识别节奏 (P5.3: 实时模式)。
//   实测单次耗时: 丢帧 ~0.3s + 定位 0.12~0.16s + 预处理 ~0.01s + 推理 ~0.40s ≈ 0.8~0.9s
//   => 周期只要小于单次耗时, 就是"背靠背连续识别", 也就是实时 (~1.2 次/秒)。
//   想放慢就调大 (5000 = 每 5s 一次); 改成 0 = 完全不自动跑, 只在按 BOOT 时识别一次。
//   注意: 上限就是推理的 0.40s, 想更快只能换更小的模型, 调这个参数没用。
static const uint32_t AUTO_PERIOD_MS = 200;

// P5.24: 连续模式下"结果可疑就自动补打一行诊断", 不用按 BOOT。
//   为什么需要: 连续模式每帧只打 2 行是刻意的(掩码/缩略图那两块 ASCII 图才是刷屏元凶),
//   但"什么都不打"会让人正好错过出问题的那一帧。这里按"可疑"触发, 并限流 4 s 一次。
//   要看完整的掩码 + 94x24 缩略图, 还是长按 BOOT。想彻底关掉自动诊断就改成 0。
#define AUTO_DIAG_ON_SUSPECT 1

// 训练/校验用的图像归一化参数 (见文件头「预处理必须与训练一致」)
static const float NORM_MEAN = 127.5f;
static const float NORM_STD = 128.0f;

// 与 data/load_data.py 的 CHARS 逐字对齐
static const char *CHARS[NUM_CLASS] = {
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "I", "O", "-",
};

// ==================== 摄像头引脚 (照抄 PlateRecognition.ino, 板子: ESP32-S3-EYE) ====================
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

#define BOOT_BTN_PIN     0   // GPIO0 = 板载 BOOT 按钮
#define BOOT_DBLCLICK_MS 400 // P5.35: 单击后等这么久, 期间又来一下 = 双击 (P5.61 起 = 复位图片输出)
#define BOOT_DEBOUNCE_MS 50  // P5.41: 离上一个被接受的边沿不到这么久 = 触点回弹, 丢掉

// BOOT 按钮用中断锁存 (P5.3): 连续识别时每轮要跑 ~0.9s, 如果只在两轮之间轮询电平,
// 用户"短按一下"很容易正好落在忙的那 0.9s 里被漏掉 (以前周期 5s、空闲 3.3s, 所以没暴露)。
// 用边沿中断把"按过"这件事记下来, 循环下一圈再消费 —— 哪怕只按 10ms 也不会丢。
// P5.15: 用 ANYEDGE 顺便量按下时长, 区分短按(绿框开关) / 长按(详细模式)
static volatile bool boot_btn_latched = false;
static volatile int32_t boot_press_ms = 0;       // 按下时刻 (ms, 32 位: ISR 与主循环共享也读不坏)
static volatile int32_t boot_release_ms = 0;     // 松手时刻 (ms), 0 = 还按着
// P5.36: 双击不能用"主循环里等 400ms 看有没有第二下"来判 —— 连续识别模式下主循环每轮要忙 ~0.5~1s,
//   用户两下都在忙的时候按完, 等主循环回来时 ISR 里只剩"最后一次按下", 双击和单击长得一模一样
//   (实测就是这样: 双击被当成单击, 变成切图片模式)。改成在 ISR 里直接数"这一串一共按了几下":
//   两下的间隔 <= BOOT_DBLCLICK_MS 就算同一串 —— 主循环再忙也漏不掉。
static volatile uint8_t boot_burst_n = 0;        // 这一串已经按下的次数
static volatile int32_t boot_last_press_ms = 0;  // 这一串上一下的时刻 (用来判断"隔太久就另起一串")
static bool boot_was_down = false;               // P5.41: 上一次接受的边沿之后的电平
static int32_t boot_last_edge_ms = 0;            // P5.41: 上一个被接受的边沿时刻
// P5.41: 去抖。机械按钮的触点回弹会在"一次按压"里再产生好几个边沿 —— 现场日志
//   (f074024f, 2026-09-30) 里用户按三下, ISR 数出 2 下(三击被当成双击, 档位没切)或者 6 下。
//   两条规则:
//     ① 电平跟上次接受的一样 => 重复边沿, 直接丢;
//     ② 离上一个被接受的边沿 < BOOT_DEBOUNCE_MS => 触点抖动, 丢 (并且**不更新** boot_was_down,
//        所以紧接着那个真正的松开边沿仍然会被接受, 不会把"还按着"这个状态卡住)。
static void IRAM_ATTR boot_btn_isr(void *arg) {
    (void)arg;
    const int32_t now = (int32_t)(esp_timer_get_time() / 1000);
    const bool down = (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0);
    if (down == boot_was_down) return;
    if (now - boot_last_edge_ms < BOOT_DEBOUNCE_MS) return;
    boot_was_down = down;
    boot_last_edge_ms = now;
    if (down) {                                            // 按下
        if (now - boot_last_press_ms > BOOT_DBLCLICK_MS) boot_burst_n = 0;   // 隔太久 => 另起一串
        boot_burst_n = (uint8_t)(boot_burst_n + 1);   // C++20 起 volatile ++ 被弃用, 写成赋值
        boot_last_press_ms = now;
        boot_press_ms = now;
        boot_release_ms = 0;
        boot_btn_latched = true;
    } else {                                               // 松手
        boot_release_ms = now;
    }
}

/**
 * P5.24: 解码结果 + 置信度明细。
 *   为什么需要: 之前日志只有一个结果字符串, 分不清两种情况 ——
 *     "图糊/框歪 => 模型自己也在犹豫" (该改定位/预处理), 与
 *     "图很干净但模型没见过这种字形 => 自信地认错" (只能靠素材微调)。
 *   把每一步的 top1 概率和"次选是谁"打出来, 这两种情况就能一眼分开。
 */
typedef struct {
    std::string text;      // CTC 折叠后的文本 (与 predict.py 的 greedy_decode 同逻辑)
    std::string seq;       // 18 步原始 argmax 拼成的串 (空白记作 "_", 不做任何合并)
    std::string steps;     // 每步 "字符 概率%(次选 概率%)" 明细
    float mean_top1;       // 非空白步的平均 top1 概率 (0..1)
    float min_top1;        // 非空白步里最低的 top1 概率
    int   min_top1_t;      // 上面那一步的下标 (-1 = 一个非空白步都没有)
    int   nseg;            // 折叠后的段数 = 识别出的字符数
    int   nchar_steps;     // 非空白的步数
    int   ambig;           // "领先 <10%" 的步数 —— 模型在这些步上没底
} decode_report_t;

/**
 * CTC 贪婪解码, 逻辑与 predict.py 的 greedy_decode 严格一致。
 * rep 非空时顺带填好置信度明细 —— 解码出的文本与不填时逐字节相同。
 * 概率 = softmax(该步 68 类输出)。⚠ 必须先减最大值再 expf: 输出 logits 的跨度实测可达
 * 100 上下, 直接 expf 会溢出成 inf, 概率就全成 NaN 了。
 */
static std::string greedy_decode_impl(const float *logits, decode_report_t *rep) {
    const int blank = NUM_CLASS - 1;
    int   labels[TIME_STEPS];
    float p1v[TIME_STEPS], p2v[TIME_STEPS];
    int   nxt[TIME_STEPS];           // 次选类别 (看 top1 是不是"勉强"赢的)

    for (int t = 0; t < TIME_STEPS; t++) {
        float vmax = logits[t];
        for (int c = 1; c < NUM_CLASS; c++) {
            const float v = logits[c * TIME_STEPS + t];
            if (v > vmax) vmax = v;
        }
        float sum = 0.0f, e1 = 0.0f, e2 = 0.0f;
        int best = 0, second = 0;
        for (int c = 0; c < NUM_CLASS; c++) {
            const float e = expf(logits[c * TIME_STEPS + t] - vmax);
            sum += e;
            if (e > e1) { e2 = e1; second = best; e1 = e; best = c; }
            else if (e > e2) { e2 = e; second = c; }
        }
        labels[t] = best;
        nxt[t] = second;
        p1v[t] = (sum > 0.0f) ? (e1 / sum) : 0.0f;
        p2v[t] = (sum > 0.0f) ? (e2 / sum) : 0.0f;
    }

    // ---- 折叠 (与 predict.py 一字不差) ----
    std::string out;
    int prev = labels[0];
    int nseg = (prev != blank) ? 1 : 0;
    if (prev != blank) out += CHARS[prev];
    for (int t = 0; t < TIME_STEPS; t++) {
        const int c = labels[t];
        if (prev == c || c == blank) {
            if (c == blank) prev = c;
            continue;
        }
        out += CHARS[c];
        nseg++;
        prev = c;
    }

    if (rep != nullptr) {
        rep->text = out;
        rep->nseg = nseg;
        rep->seq.clear();
        rep->steps.clear();
        rep->mean_top1 = 0.0f;
        rep->min_top1 = 2.0f;
        rep->min_top1_t = -1;
        rep->nchar_steps = 0;
        rep->ambig = 0;
        float sum_top1 = 0.0f;
        char buf[48];
        for (int t = 0; t < TIME_STEPS; t++) {
            const bool is_blank = (labels[t] == blank);
            if (is_blank) rep->seq += '_';
            else rep->seq += CHARS[labels[t]];
            const char *nm2 = (nxt[t] != blank) ? CHARS[nxt[t]] : "_";
            snprintf(buf, sizeof(buf), "%s%.0f(%s%.0f) ",
                     is_blank ? "_" : CHARS[labels[t]], p1v[t] * 100.0f, nm2, p2v[t] * 100.0f);
            rep->steps += buf;
            if (!is_blank) {
                rep->nchar_steps++;
                sum_top1 += p1v[t];
                if (p1v[t] < rep->min_top1) { rep->min_top1 = p1v[t]; rep->min_top1_t = t; }
                if (p1v[t] - p2v[t] < 0.10f) rep->ambig++;
            }
        }
        rep->mean_top1 = (rep->nchar_steps > 0) ? (sum_top1 / (float)rep->nchar_steps) : 0.0f;
        if (rep->nchar_steps == 0) rep->min_top1 = 0.0f;
    }
    return out;
}

/** 只要文本时用这个 (与预测脚本一致) */
static std::string greedy_decode(const float *logits) {
    return greedy_decode_impl(logits, nullptr);
}

/** 归一化 + 量化 LUT: lut[c*256+v] = quantize((v-mean)/std) */
static uint8_t g_inv_lut[256];   // q(-128..127) -> v(0..255)
/** 反查表: 量化后的 q -> 原来的 v. 用来把"模型真正吃到的图"还原成人能看的 uint8 (P5.16)
 *  本模型 (exponent=-7) 下 v->q 就是 clamp(v-127), 反着来是 v=q+127; 唯一的边界是
 *  q=127 同时对应 v=254 和 255, 所以从 LUT 逐格反查, 取最大的那个 v (v 递增覆盖)。 */
static void build_inv_lut(const int8_t *lut) {
    for (int i = 0; i < 256; i++) {
        int v = i - 128 + 127;                 // 兜底: v = q + 127
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        g_inv_lut[i] = (uint8_t)v;
    }
    for (int v = 0; v < 256; v++) g_inv_lut[(int)lut[v] + 128] = (uint8_t)v;
}

static void build_lut_ex(int8_t *lut, int exponent, float mean, float std) {
    const float inv_scale = 1.0f / DL_SCALE(exponent);   // exponent=-7 -> 128
    for (int c = 0; c < 3; c++) {
        for (int v = 0; v < 256; v++) {
            // 必须与 esp-dl 的 quantize<int8_t>() 逐位一致, 否则和已验证的板端 float 喂数链路差 1 个 LSB:
            //   dl/tensor/src/dl_tensor_base.cpp:8   quantize = tool::round(input * inv_scale) 然后 clip
            //   dl/tool/src/dl_tool.cpp:69-88        S3 的 tool::round = (int)floorf(value + 0.5f)  (半值向 +inf)
            //   (P4 走的是 round_half_even, 与这里不同; 本工程只跑 S3)
            // ⚠ 不要用 lroundf()/round(): 它们是"半值远离 0", 对 v<=127 会整体差 1。
            //   本模型下 (v-127.5)/128*128 恰好等于 v-127.5, 故结果就是 clamp(v-127)。
            int q = (int)floorf((v - mean) / std * inv_scale + 0.5f);
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            lut[c * 256 + v] = (int8_t)q;
        }
    }
}

/** P5.66: 省字模型的归一化 LUT (mean 127.5 / std 63.75 = 训练里的 (x/255-0.5)/0.25) */
static void build_norm_lut(int8_t *lut, int exponent) { build_lut_ex(lut, exponent, NORM_MEAN, NORM_STD); }
static void build_prov_lut(int8_t *lut, int exponent) { build_lut_ex(lut, exponent, PROV_NORM_MEAN, PROV_NORM_STD); }

// ---- P5.28: 直接读写 GC2145 寄存器 ----
// 这个驱动的 set_brightness / set_contrast / set_saturation / set_exposure_ctrl /
// set_whitebal / set_gainceiling 全是 set_dummy, 只打印一行 "Unsupported" 就返回, 什么都没干。
// 真正有效的只有 set_reg / get_reg, 但它不分页 —— 所以每次都得先往 0xfe 写页号。
#define GC_PAGE_AEC     1
#define GC_REG_AEC_TGT  0x13    // AEC 目标亮度, 驱动默认 0x40; 调小 = 整体拍暗, 用来压过曝/泛白
#define GC_AEC_TARGET   0       // 0 = 保持驱动默认; 想试就填 0x38 / 0x30 / 0x28

static int gc_rd(sensor_t *s, uint8_t page, uint8_t reg) {
    s->set_reg(s, 0xfe, 0xff, page);
    return s->get_reg(s, reg, 0xff);
}

static int gc_wr(sensor_t *s, uint8_t page, uint8_t reg, uint8_t v) {
    s->set_reg(s, 0xfe, 0xff, page);
    return s->set_reg(s, reg, 0xff, v);
}

static void camera_tune_registers(sensor_t *s) {
    if (s->id.PID != GC2145_PID) {
        return;                             // 只有 GC2145 的分页规则是确认过的
    }
    const int tgt = gc_rd(s, GC_PAGE_AEC, GC_REG_AEC_TGT);
    if (tgt < 0) {
        ESP_LOGE(TAG, "GC2145 寄存器读失败 (%d): SCCB 不通, 下面几个值都不可信", tgt);
        return;
    }
    ESP_LOGI(TAG, "GC2145 寄存器: 曝光目标(页1 0x13)=0x%02x | AEC使能(页0 0xb6)=0x%02x | 自动开关(页0 0x82)=0x%02x",
             tgt, gc_rd(s, 0, 0xb6) & 0xff, gc_rd(s, 0, 0x82) & 0xff);
    ESP_LOGI(TAG, "GC2145 AEC窗口: X1=0x%02x X2=0x%02x Y1=0x%02x Y2=0x%02x 中心权重(0x0c)=0x%02x",
             gc_rd(s, GC_PAGE_AEC, 0x01) & 0xff, gc_rd(s, GC_PAGE_AEC, 0x02) & 0xff,
             gc_rd(s, GC_PAGE_AEC, 0x03) & 0xff, gc_rd(s, GC_PAGE_AEC, 0x04) & 0xff,
             gc_rd(s, GC_PAGE_AEC, 0x0c) & 0xff);

    // 无论开不开这个开关都写一次: 写回读一致 => 证明"能真正控制这颗芯片"。
    const uint8_t want = GC_AEC_TARGET ? (uint8_t)GC_AEC_TARGET : (uint8_t)tgt;
    gc_wr(s, GC_PAGE_AEC, GC_REG_AEC_TGT, want);
    const int now = gc_rd(s, GC_PAGE_AEC, GC_REG_AEC_TGT);
    ESP_LOGI(TAG, "曝光目标 0x%02x -> 0x%02x | 写回读 0x%02x (%s)", tgt, want, now & 0xff,
             (now == (int)want) ? "读写在控, 通道没问题" : "没写进去!");

    s->set_reg(s, 0xfe, 0xff, 0x00);
}

/** 摄像头初始化: 参数与 PlateRecognition.ino 的 vCameraInit() 完全一致 */
static esp_err_t camera_start(void) {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_VGA;      // 640x480, 适合车牌识别
    config.jpeg_quality = 12;               // RGB565 用不到, 保持默认
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init 失败: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "sensor 为空");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "摄像头 PID=0x%x", (unsigned)s->id.PID);

    camera_tune_registers(s);
    if (s->id.PID == OV2640_PID || s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
    } else if (s->id.PID == GC032A_PID) {
        s->set_vflip(s, 1);
    } else if (s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);
    }
    return ESP_OK;
}

/** 丢弃前 n 帧, 让自动曝光/白平衡收敛 (与 PlateRecognition.ino 的拍照流程一致) */
// 丢帧数: 每次识别前丢 CAM_DISCARD_FRAMES 帧, 保证拿到的是"刚拍的新帧"而不是缓冲里的旧帧;
// 摄像头刚启动/重启时用 CAM_WARMUP_FRAMES 多丢一些, 等 AE/AGC 稳定 (否则第一帧偏暗)。
#define CAM_WARMUP_FRAMES    12
#define CAM_WARMUP_DELAY_MS  100   // 开机那一次: 慢慢丢, 给 AE/AWB 时间收敛
// P5.27: 每次识别前丢几帧 / 丢帧之间等多久。
//   原来这里固定 vTaskDelay(100) x 3 = **整整 300 ms 纯等待**, 占单帧约 1.3 s 的 23%。
//   它是从旧的"拍一张照"流程照搬来的 (PlateRecognition.ino: 丢 10 帧 + delay(100)) ——
//   那边是"初始化相机 -> 拍一张", 所以必须等 AE 收敛; 这里相机是**一直流**的:
//   esp_camera_fb_get() 本身就会阻塞到有新帧, 丢 3 帧天然前进 3 个帧周期,
//   而 AE/AWB 在这一秒多的循环里早就跟上了, 根本不需要再空等。
//   万一发现"刚移动相机后的头一两帧偏亮/偏暗", 把 CAM_DISCARD_DELAY_MS 改回 100 即可。
#define CAM_DISCARD_FRAMES    3
#define CAM_DISCARD_DELAY_MS  0

static void camera_discard_frames(int n, int delay_ms) {
    for (int i = 0; i < n; i++) {
        camera_fb_t *tmp = esp_camera_fb_get();
        if (tmp) {
            esp_camera_fb_return(tmp);
        }
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

static void log_input_stats(const dl::TensorBase *t) {
    const int8_t *p = (const int8_t *)t->data;
    size_t n = 1;
    for (size_t i = 0; i < t->shape.size(); i++) {
        n *= t->shape[i];
    }
    int lo = 127, hi = -128;
    long sum = 0;
    for (size_t i = 0; i < n; i++) {
        int v = p[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    ESP_LOGI(TAG, "输入张量 int8: min=%d max=%d mean=%.1f (n=%u)",
             lo, hi, (float)sum / (float)n, (unsigned)n);
}

// P5.26: 这里原本有两个东西, 都删了。
//   1) "裁剪块 1:1 缩略图" —— 94 字符宽 x 24 行的 ASCII 灰度画。实测没人看得清: 94 列在任何
//      终端/串口助手里都会折行, 一旦折行就没法对着字认了。而它想回答的问题
//      ("这块图糊不糊/过曝不过曝/颜色对不对") 现在全部有数字答案:
//      均值 B/G/R、亮度跨度、过曝/死黑占比、锐度、每步置信度。
//      真要看"模型到底吃了什么", 用短按 BOOT 切到模式 2 —— 串口会直接发那张 94x24 的真 JPEG 图,
//      既能肉眼看, 又能存下来当训练素材, 比字符画强得多。
//   2) crop_snap —— 每帧把输入张量拷 6.8 KB 到 PSRAM。它是**死代码**: 只在 !verbose 时分配,
//      唯一使用它的地方却要求 verbose, 于是永远是 nullptr, 白拷了一整轮、一次都没打出来过。

/** 结果格式粗校验: 省份简称(1 个汉字) + 1 个字母 + 5~6 个数字/字母
 *  等价于 auto_crop_predict.py 里的 PLATE_RE, 用来把"明显跑偏的输出"挑出来。 */
static bool plate_looks_valid(const std::string &s) {
    // UTF-8 下汉字占 3 字节, 其余都是单字节 => 7 位车牌 9 字节, 8 位车牌 10 字节
    if (s.size() != 9 && s.size() != 10) return false;
    bool prov_ok = false;
    for (int i = 0; i < 31; i++) {
        if (strncmp(s.c_str(), CHARS[i], 3) == 0) {
            prov_ok = true;
            break;
        }
    }
    if (!prov_ok) return false;
    for (size_t i = 3; i < s.size(); i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

// ==================== 车牌 ROI 定位 ====================
// 移植自训练工程 G:\All_Project\AI_Project\LPRNet_Pytorch\auto_crop_predict.py (那套阈值和"去边比例"是在 PC 上调过的):
//   BGR->HSV -> 蓝/绿阈值 -> 形态学 闭(17x5)+开(17x5) -> 轮廓 -> 最小外接矩形 -> 面积/宽高比打分 -> 透视摆正 + 去边
// 板端简化: 判定阈值一字不改, 只降实现代价
//   * 在 1/4 粗网格上跑 (640x480 -> 160x120), 每格统计"车牌色像素是否 >=25%"
//   * 闭/开运算结构元退化成 4x1 (对应原图 17x5)
//   * 轮廓->连通域(union-find); 最小外接"旋转"矩形退化成轴对齐外接框
//     => 车牌倾斜明显时框里会带背景, 属已知折中, 见文档 12.6
#define ROI_GRID_STEP 4
#define ROI_GRID_W (640 / ROI_GRID_STEP)   // 160
#define ROI_GRID_H (480 / ROI_GRID_STEP)   // 120
#define ROI_GRID_N (ROI_GRID_W * ROI_GRID_H)

static const float ROI_MIN_AREA_RATIO = 0.002f;   // auto_crop_predict.py: area < img_area*0.002 丢弃
// P5.60: 2.0~6.5 -> 2.2~4.2。
//   真车牌本体(旋转拟合后)长宽比 3.1~3.4, 4.2 已经比它宽 25%, 够宽容了;
//   而 4.2~6.5 那一档收进来的全是"车牌 + 旁边一条蓝色背景"的连体块 —— 复算显示它对识别没贡献、只有干扰。
static const float ROI_RATIO_LO = 2.2f;
static const float ROI_RATIO_HI = 4.2f;
static const float ROI_RATIO_IDEAL = 3.4f;        // auto_crop_predict.py: 打分基准
// P5.2 有效填充率下限。有效填充率 = 能摆正 => 旋转矩形填充率(与倾角无关); 否则 => 轴对齐外接框密度。
// 依据 (2026-09-28 新固件日志, 10 次会话): 真车牌 65%~90% 全对/接近; 散块噪块 35%~54% 全乱。
// 离线自检 tools/roi_geom_selftest.ps1: 直立 88%, 倾斜 20/25/33 度 85/85/84%。
// ⚠ 不能拿轴对齐密度一刀切: 倾斜车牌的外接框密度只有 40%~47% (离线 F/G/H), 那样会误杀真车牌。
static const float ROI_MIN_FILL = 0.60f;
// P5.46: 兜底档 —— **面积够了就收, 比例和填充一概不看**。
//   为什么改成这样 (P5.45 的教训): 屏幕反光会把蓝底打散, 连通域拼不出"车牌形状", 严格档的
//   比例 2.0~6.5 / 填充 >=60% 就会一起把它否掉 —— 而它确实是车牌。
//   实测 (2026-10-01 的 78 帧日志): 21 个错结果**全部**来自占屏 0.1%~1.3% 的碎块, 形状门槛拦不住它们;
//   唯一正确的 #82 反而是最大的一块 (占屏 43.7%)。=> 形状当不了裁判。
// 代价靠"结果闸门"兜: 结果必须先过 plate_looks_valid() (7~8 位, 省字开头) 才算车牌, 否则只打一行疑似误检。
// 面积底线与严格档相同 (38 格) —— 只放开形状, 不放开"小碎点"。
// 旋转拟合内部门槛 0.45 -> 0.35: 让被反光打散的斜牌也能走"摆正"路径。
static const float ROI_FIT_MIN_FILL = 0.35f;
static const float ROI_TRIM_X = 0.01f;            // auto_crop_predict.py: 左右各去 1%
static const float ROI_TRIM_Y = 0.03f;            // auto_crop_predict.py: 上下各去 3%
// P5.49: 亮度下限 V —— 46 -> 120。
//   现场 (2026-10-01 用户 20 张难帧, 白天拍屏幕): 屏幕那层深蓝底色和车牌蓝的**色相、饱和度都重叠**
//   (底色 H 200~260° / S 能到 70+; 车牌 H 200~260° / S 60~130), 唯一分得开的是**亮度**:
//   车牌蓝底的最亮通道约 150~200, 而屏幕底色只有 60~90。原来的 46 等于把整屏底色全放了进来,
//   于是掩码糊满画面、连通域涨到占屏 52%~90%, 严格档拒不掉、兜底档整个收下 -> 框 = 一整屏。
//   PC 复算 (tools/locator_replay.py --v-min, 20 张难帧): 只动这一个数 ——
//     V=46 : 『像车牌的框』0/20,  巨框(占屏>45%) 18/20
//     V=120: 『像车牌的框』15/20, 巨框 0/20      (饱和度一个字没改)
//   代价: 很暗的车牌(B 通道掉到 ~100)会被误杀 —— 那时该帧打印"画面里没有一块车牌色"直接跳过,
//   比"喂一坨整屏背景给模型"好得多。若以后主要用在暗环境, 再考虑改成跟着整帧亮度自适应。
static const int ROI_MIN_VALUE = 120;
// P5.60: 蓝牌判据换成"蓝通道明显高于红通道"。
//   依据 (2026-10-01, 用户新拍的 171 张原始帧: 京Q06666 / 豫FSQ818 / 豫A8F8Q8, 白天屏摄+反光):
//     只把蓝牌判据从"色相 180~270 + 饱和度>=35"换成 "(b-r)>40 且 b>120", 其余机器一字不动,
//     端到端整串全对 28/171 -> 96/171 (定位机器、比例/填充门槛、裁剪几何全部没变)。
//   为什么: 屏摄场景里反光/屏幕底色会把色相和饱和度一起搅乱(同一块牌的不同像素色相能差几十度),
//     而"蓝比红高多少"几乎不受影响 —— 蓝牌底 b-r 中位约 107, 屏幕泛蓝的底色只有 20~40。
//   注意 b > ROI_MIN_VALUE 这一半是给绿牌留的: 深绿像素的 b 只有 60~90, 过不了这条, 不会误进蓝牌分支。
static const int ROI_BLUE_BR_MIN = 40;
// P5.49: 候选占屏上限。严格档和兜底档都不收超过它的块。
//   依据同上: 真车牌本体占屏 <=~32%, 被反光/背景污染的块 52%~90%。这一刀从源头掐掉"巨框"。
static const float ROI_MAX_AREA_PCT = 45.0f;
// P5.56/P5.58: **占屏下限**, 严格档和兜底档都生效。面积上下限是一对, 放在一起看:
//   上限 45% 治"整屏背景被收下", 下限治"碎块被当成车牌候选"。
//   原来的下限只有 38 格(0.2%), 太低 —— 实测两种碎块都能溜过去:
//     兜底档: 84x16 px(占屏 0.4%、长宽比 5.25)   -> 模型吐出一个孤零零的省字
//     严格档: 76x16 px(占屏 0.4%、长宽比 4.75)   -> 比例和填充居然都过, 同样吐一个省的
//   取 2.0%: 能用的距离内真车牌至少占屏 4%(190x76 px, 见文档四十二节 档3 的 92%), 2% 留一倍余量,
//   既拦掉 0.1%~1.3% 那一带碎块, 又不至于把"稍远但还看得清"的牌误杀。
static const float ROI_MIN_AREA_PCT = 2.0f;

// 采样时的"外扩留白"。训练素材里字符不顶边(见 data/official_val 的 94x24 图),
//   而我们的框是"蓝区紧贴边"再去 1%/3% 边 => 不补一点留白, 最后一个字会被挤掉。
//
// P5.13 的老结论(同一块牌 35 帧, 浮点模型离线跑): 对称外扩 0% -> 2/35 正确(30 帧少最后一个 6),
//   4% -> 10/35, 8% -> 28/35(当初取这个), 10% -> 18/35, 12% -> 0/35。
//
// P5.32 修正: 那个 8% 是**对称**的, 而左右两端并不对称 —— 左边多出来的那点内容会**多认一个省份字**。
//   板上日志的铁证 (46 帧实拍, 京Q06666): 17 帧输出 "沪京Q06666"/"浙京Q06666"(多一个字),
//   而且 CTC 时间步全是 _沪京_Q__0_6_6_6_6__ —— 第一个字被拆成了 沪(第1步) + 京(第2步);
//   只有当它连着输出 京京 时才会被 CTC 折叠成正确的 京 (见帧 #80 _京京_Q__D_6_6_6_6__)。
//   机理: 长边两端各外扩 8% ≈ 94 px 输入里 7.5 px, 再加框本身比蓝面宽 2~4 px, 左边共约 10 px
//         ≈ 正好一个字符格 —— 第 1 个时间步整格压在"牌子外面那点东西"上。
//   PC 复算 (65 帧实拍, 浮点模型, 只动长边两端外扩):
//     左 7% / 右 7% (=旧值) -> 正确  0~3,  多字  8~18     <= 就是日志里那个毛病
//     左 4% / 右 7%         -> 正确 10~14, 多字  2~6
//     左 0% / 右 4%         -> 正确 29~30, 多字  0~1      <= 取这个
//     上下外扩 0/3/5% 对结果几乎没影响(29/28/30), 所以短边维持原样。
//   为什么右边要留 4% 而左边收到 0: 右边不留白会切掉最后一个字(就是 P5.13 那 30/35 帧的毛病),
//   左边不留白则正好把"牌子外面那点东西"挤出去。
//   净外扩 = 本值 - ROI_TRIM_X(先按 1% 收过边), 所以右 0.05-0.01=4%。
//
// P5.38 (2026-09-30) 把左端从净 0% 改成净 +2% —— 回来一看, P5.32 的"左端收到 0"收过头了。
//   证据: 串口助手存下来的 165 张真实 94x24 模型输入(4 块牌), 浮点模型离线重跑:
//     基线 (净左 0% / 右 4%)     京Q 45%  粤T 57%  豫J 85%  豫A  75%   合计  89/165 = 53.9%
//     左端再 +1%                 京Q 54%  粤T 60%  豫J 92%  豫A  88%   合计 100/165 = 60.6%
//     左端再 +2%  (本次采用)     京Q 67%  粤T 48%  豫J 92%  豫A 100%   合计 105/165 = 63.6%
//     左端再 +3% / +4%           京Q 70%  粤T 48%  豫J 92%  豫A 100%   合计 108/165 = 65.5%
//     左端再 +5%                 合计  86/165 = 52.1%   (再大就开始崩)
//   奇偶分半复核(每半 82~83 张): 基线 57% / 51% -> +2~4% 两半都是 63~66% / 65%,
//     不是单批巧合。代价: 粤T666FP 那块在 +2% 以上会掉(57% -> 48%), 其余三块都明显涨。
//   为什么: 改之前左右不对称(左净 0 / 右净 4), 字符整体偏在 94px 的左边;
//     离线单独测"把内容在 94px 里整体右移" 0px -> 53.9%, +2~3px -> 67.3%, 左移一律崩。
// P5.39: 左端留白改成运行时可切 (BOOT 三击), **默认回到 P5.37 的老值(净 0%)**。
//   P5.38 曾经直接把它定成 0.03f(=净 +2%) —— 离线在 165 张真实 94x24 上它是涨的
//   (53.9% -> 63.6%, 奇偶分半也复现), 但真机上用户反馈"京Q 反而更糟"。两边对不上, 就不猜了:
//   默认保守(净 0% = P5.37 行为), 想试 +2% 就现场按三下 BOOT 切过去, 串口会打当前生效值。
// P5.40: "取样几何档" —— BOOT 三击循环。左右留白越大, 内容在 94px 里越小、四周的余量越多。
static const float ROI_PAD_U_L_OFF = 0.01f;   // 净 0%  (P5.30 起的老值, P5.37 的行为)
static const float ROI_PAD_U_L_ON  = 0.03f;   // 净 +2% (P5.38 试过的值)
static const float ROI_PAD_U_L_IN  = -0.03f;  // 净 -4% (P5.43 新: 往框里收, 治定位框比真车牌宽)
static const float ROI_PAD_U_R_OFF = 0.05f;   // 长边右端 净 4% (P5.32 起的老值)
static const float ROI_PAD_U_R_ON  = 0.08f;   // 长边右端 净 7% (P5.13/P5.21 老几何)
static const float k_pad_profiles[4][2] = {
    {ROI_PAD_U_L_OFF, ROI_PAD_U_R_OFF},   // 档0 左净 0%  / 右净 4%  (P5.32~P5.39 老几何)
    {ROI_PAD_U_L_ON,  ROI_PAD_U_R_OFF},   // 档1 左净 +2% / 右净 4%  (P5.38: 真机 330~420px 83%)
    {ROI_PAD_U_L_ON,  ROI_PAD_U_R_ON},    // 档2 左净 +2% / 右净 +7%  <- P5.61 默认
    {ROI_PAD_U_L_IN,  ROI_PAD_U_R_ON},    // 档3 左净 -4% / 右净 +7%
};
// P5.42: 默认档改成 **档3**(左右各净 +7%)。四份真机日志按"档 x 画面里的车牌宽度"合并
//   (只数 京Q06666 这块牌, 结论见 三十九 节):
//     档0  124 帧 22 对 = 18%    (有量的那一段 330~420px: 114 帧 13 对 = 11%)
//     档1   40 帧 32 对 = 80%    (330~420px: 30 帧 25 对 = 83%)
//     档2   53 帧 26 对 = 49%    (<250px: 33 帧 13 对 = 39%)
//     档3   27 帧 23 对 = 85%    (<250px: 12 帧 **11 对 = 92%**; 330~420px: 13 帧 10 对 = 77%)
//   关键在最后一行: 牌小到 190x76 px(只占屏 4%)时, 档3 还能 12 帧对 11 帧 —— 这正是用户要的
//   "恰当距离内都能识别"。档1 在 330~420px 与档3 打平(83% vs 77%, 样本都小), 但它在 <330px
//   那两段**一帧数据都没有**; 而档3 的 3 个错法全是"两端各多一个字"(京R京Q06666L 之类) ——
//   (那原本是"长边收边"该管的 —— 但 P5.61 复算证明收边在切省字, 已整段删除, 见文档第四十五节。三击仍可现场切回任何一档。)
// P5.43: 默认档 3 -> 2, 并把 档2/档3 换成 左右不对称(左小右大) 的组合。
//   起因(2026-09-30 用户日志): 档3(左右各 +7%)把 左边凭空多认一个省字 放回来了 ——
//   61 帧结果里 38 帧带一个多出来的省字(浙 34 / 闽 4 / 沪 1), 置信 93~100%, 京Q06666 一帧没全对。
//   机理就是 P5.32 那条(三十四节): 长边左端外扩过多 -> 第一个时间步压在牌子外面 -> CTC 多吐一个字。
//   结论: 左边要 小 (防多字), 右边要 大 (防手持抖动把末尾字符切出窗)。
// P5.61: 左边留白 净 0% -> 净 +2% (档2 的左端从 ROI_PAD_U_L_OFF 换成 ROI_PAD_U_L_ON)。
//   171 帧原始帧复算(长边收边已删): 左 +1% 128/171 -> 左 +3% **138/171 = 81%** -> 左 +5% 117/171;
//   而左 -1% 只有 68/171。左端**必须**留白, 3% 附近是个清晰的峰 —— 与 P5.38 的结论同向。
#define PAD_DEFAULT_PROFILE 2
static int g_pad_profile = PAD_DEFAULT_PROFILE;
static float g_pad_u_l = k_pad_profiles[PAD_DEFAULT_PROFILE][0];
static float g_pad_u_r = k_pad_profiles[PAD_DEFAULT_PROFILE][1];
// P5.61: 0.08 -> 0.12 (净 5% -> 净 11%)。同一批复算: V 0.05 -> 134/171, 0.08 -> 138/171,
//   0.12 -> **142/171 = 84%**, 0.15/0.18/0.22 -> 140/139/141 —— 0.12~0.22 是一条平顶,
//   取 0.12(实测最好); 上下多留一点对"牌顶被反光吃掉"也更宽容。
static const float ROI_PAD_V   = 0.12f;
static void pad_apply_profile(int k) {
    if (k < 0) k = 0;
    if (k > 3) k = 3;
    g_pad_profile = k;
    g_pad_u_l = k_pad_profiles[k][0];
    g_pad_u_r = k_pad_profiles[k][1];
}

#define ROI_MAX_SLOTS 32      // 连通域上限 (原实现就是 32)
#define ROI_CELL_NEED ((ROI_GRID_STEP * ROI_GRID_STEP + 3) / 4)   // 一格内 >=25% 像素是车牌色才算数
#define ROI_MASK_BUDGET 3     // 只在"详细模式"(开机 / 按 BOOT) 的前几次会话打印掩码: 一张 80x60 掩码 = 4800 字符 ≈ 420ms @115200, 连续模式下会拖慢循环

// 最近一次 roi_locate 算出的"车牌色格"快照 (PSRAM)。roi_locate 返回后就地复用它,
// 让 recognize_once 能在"结果不像车牌"时补打一次掩码。
static uint8_t *roi_cell_snap = nullptr;

typedef struct {
    int x1, y1, x2, y2;   // 原图像素坐标, [x1,x2) x [y1,y2)
    float ratio;          // 宽高比
    float area_pct;       // 占整帧百分比
    float score;
    float density;        // 有效填充率 0..1 (能摆正=旋转矩形填充率, 否则=轴对齐密度); 真车牌 >=0.65
    // --- P5.1: PCA 拟合出的旋转矩形 (等价 cv2.minAreaRect), 用来把倾斜的车牌摆正 ---
    bool  rot_ok;         // false = 拟合不合格, 退回轴对齐裁剪 (dl::image::resize)
    float rot_ratio;      // 旋转矩形的长宽比 (真车牌本体约 3.1~3.4)
    float rot_fill;       // 旋转矩形内的填充率 (低 => 连通域是散块/被背景撑大)
    float rot_deg;        // 相对水平方向的倾角, 单位度
    float qx[4], qy[4];   // 摆正用四角(原图像素): 左上/右上/右下/左下, 已按原脚本比例去边
    const char *rot_why;  // P5.24: 拟合失败的具体原因 ("旋转长宽比越界" 等), 供日志/自动诊断用
} roi_box_t;

/** P5.29/P5.31: 用"这张图自己的主色"重算一遍蓝面框 —— 只打日志, 不参与裁剪, 零风险。
 *  动机: 现在的蓝面判定是一条**写死的门槛** (色相 180~270 且 S>=35)。屏幕上泛蓝的背景和白边
 *  刚好越过这条死线, 于是被当成牌面一起框进裁剪块; 在 94 px 宽的模型输入里, 左边多出几个像素的
 *  亮内容, 模型就会多认一个汉字 —— 日志里的 沪京Q06666 / 辽京Q06666 就是这么来的。
 *  这里换成**活门槛**: 统计 (B-G, G-R) 二维直方图, 最大的一坨就是牌底色; 再取"颜色离它够近"的
 *  像素的外接框。跟 auto_crop_predict.py 的 find_plate_quad 相比, 只把固定的 HSV 门槛换成跟着
 *  这张图走的主色, 后面的形态学/打分/摆正一个字不改。
 *
 *  P5.31 两处修正 (P5.29 那版在板上永远打"没有明显的主色蓝, 跳过"):
 *   1) 统计范围从**整帧**收到**选中的框以内**。实测整帧里约 29% 的像素都偏蓝 (屏幕底色/反光),
 *      在整帧上求外接框等于没框, 差值全是假的; 只有框内那一小块的主色才是牌底。
 *   2) 峰值从**单格**计数改成 **3x3 邻域求和**。实测单格中位只占 3.0% (颜色被噪声摊到邻近好几格),
 *      永远过不了 n/20=5% 的门槛; 3x3 之后框内中位 17.9%, 门槛才有意义。
 *  离线对照 (66 帧实拍, 套在参考框上): 框内浓度中位 17.9% (7.3~28.3%), 左/右各可收 3.6% ≈
 *  94 px 输入里 3.4 px —— 正好对上"左边多 3~4 px 亮内容就多认一个汉字"的实验结论。*/
#define DBIN_SHIFT  3
#define DBIN_N      (256 >> DBIN_SHIFT)        // 32 档
static void roi_probe_dominant_blue(const uint8_t *rgb565be, int w, int h, const roi_box_t *box) {
    int x1 = box->x1, y1 = box->y1, x2 = box->x2, y2 = box->y2;
    if (x1 < 0) x1 = 0;                        // 车牌贴边时外接框会是负的, 不夹住切片就翻车
    if (y1 < 0) y1 = 0;
    if (x2 > w) x2 = w;
    if (y2 > h) y2 = h;
    const int bw = x2 - x1, bh = y2 - y1;
    if (bw < 8 || bh < 4) { ESP_LOGW(TAG, "主色诊断: 框太小 (%dx%d), 跳过", bw, bh); return; }

    static uint16_t hist[DBIN_N * DBIN_N];     // 2 KB
    memset(hist, 0, sizeof(hist));
    long n = 0;
    for (int y = y1; y < y2; y += 2) {          // 隔行隔列: 与 PC 对照实验同样的采样密度
        const uint8_t *row = rgb565be + (size_t)y * w * 2;
        for (int x = x1; x < x2; x += 2) {
            const uint8_t b0 = row[x * 2], b1 = row[x * 2 + 1];
            const int r = b0 & 0xF8;
            const int g = (((b0 & 0x07) << 5) | ((b1 & 0xE0) >> 3)) & 0xFF;
            const int b = (b1 & 0x1F) << 3;
            const int bg = b - g, gr = g - r;
            if (bg <= 0 || gr < 0) continue;    // 不够蓝的、偏红的都不进统计
            int i = bg >> DBIN_SHIFT; if (i >= DBIN_N) i = DBIN_N - 1;
            int j = gr >> DBIN_SHIFT; if (j >= DBIN_N) j = DBIN_N - 1;
            hist[i * DBIN_N + j]++;
            n++;
        }
    }
    if (n < 100) { ESP_LOGW(TAG, "主色诊断: 框内偏蓝像素太少 (%ld), 跳过", n); return; }

    long bc = 0; int bi = 0, bj = 0;            // P5.31: 峰值取 3x3 邻域求和, 不是单格
    for (int i = 2; i < DBIN_N; i++) {          // i<2 => B-G<16, 不可能够蓝
        for (int j = 0; j < DBIN_N; j++) {
            long s = 0;
            for (int di = -1; di <= 1; di++) {
                const int ii = i + di; if (ii < 0 || ii >= DBIN_N) continue;
                for (int dj = -1; dj <= 1; dj++) {
                    const int jj = j + dj; if (jj < 0 || jj >= DBIN_N) continue;
                    s += hist[ii * DBIN_N + jj];
                }
            }
            if (s > bc) { bc = s; bi = i; bj = j; }
        }
    }
    const float conc = 100.0f * (float)bc / (float)n;
    if (bc < n / 20) {                          // 板上实测框内中位 17.9%, 5% 已是很松的门槛
        ESP_LOGW(TAG, "主色诊断: 框内没有明显的主色蓝 (3x3 最大只占 %.1f%%), 跳过", conc);
        return;
    }
    const int bg0 = bi << DBIN_SHIFT, gr0 = bj << DBIN_SHIFT;
    int fx0 = bw, fy0 = bh, fx1 = -1, fy1 = -1; // 坐标都相对框左上角
    long hit = 0;
    for (int y = y1; y < y2; y += 2) {
        const uint8_t *row = rgb565be + (size_t)y * w * 2;
        for (int x = x1; x < x2; x += 2) {
            const uint8_t b0 = row[x * 2], b1 = row[x * 2 + 1];
            const int r = b0 & 0xF8;
            const int g = (((b0 & 0x07) << 5) | ((b1 & 0xE0) >> 3)) & 0xFF;
            const int b = (b1 & 0x1F) << 3;
            const int bg = b - g, gr = g - r;
            if (bg < bg0 - 8 || bg > bg0 + 8) continue;   // 离主色 +-1 档以内
            if (gr < gr0 - 8 || gr > gr0 + 8) continue;
            const int lx = x - x1, ly = y - y1;
            if (lx < fx0) fx0 = lx;
            if (lx > fx1) fx1 = lx;
            if (ly < fy0) fy0 = ly;
            if (ly > fy1) fy1 = ly;
            hit++;
        }
    }
    if (fx1 < 0) { ESP_LOGW(TAG, "主色诊断: 主色像素外接框为空"); return; }

    const int dl = fx0, dr = (bw - 1) - fx1, dt = fy0, db = (bh - 1) - fy1;
    ESP_LOGI(TAG, "主色诊断: 牌底色 B-G≈%d G-R≈%d | 3x3 浓度 %.1f%% | 主色框 %dx%d 在框内偏移 (%d,%d) | 命中 %ld/%ld",
             bg0 + 4, gr0 + 4, conc, fx1 - fx0 + 1, fy1 - fy0 + 1, fx0, fy0, hit, n);
    ESP_LOGI(TAG, "主色诊断: 死门槛框 %dx%d | 差值 左%d 右%d 上%d 下%d px (占宽 %.1f%% / %.1f%%) | 折算 94x24 输入 = 左 %.1f 右 %.1f px",
             bw, bh, dl, dr, dt, db, 100.0f * (float)dl / (float)bw, 100.0f * (float)dr / (float)bw,
             94.0f * (float)dl / (float)bw, 94.0f * (float)dr / (float)bw);
}
// ==================== P5.12: 串口图像输出 (给「BY串口助手」实时预览) ====================
// 协议 (与 BY串口助手/serial_img_tool.py 的「二进制帧」模式逐字节对齐):
//     "$IMG,<len>\r\n" + JPEG 二进制(恰好 len 字节) + CRC32(大端 4 字节) + "$END\r\n"
//   - 行尾的 \r 无所谓: 助手对帧头/帧尾都做了 strip("\r\n")
//   - 二进制段必须绕开 stdout 的换行转换, 见 img_tx_raw_write 的注释
//   - len 是十进制字节数; CRC32 = zlib crc32, 即 Python 的 binascii.crc32(img) & 0xFFFFFFFF
//   - 工具端 CRC 不过会整帧丢弃。所以从写 header 到 fflush 之间**绝对不能打任何 LOG**,
//     否则那几行文字会被当成图片数据, 整帧作废。
//
// 为什么要绕这么大一圈:
//   1) GC2145 驱动只能出 RGB565 / YUV422, 出不了 JPEG (见 components/esp32-camera/sensors/gc2145.c);
//   2) esp_new_jpeg 的编码器不吃 RGB565, 只吃 RGB888/GRAY/YCbYCr -> 必须自己转;
//   3) 640x480 RGB565 = 614 KB, 在 115200 上要 53 秒 -> 先 2x2 平均缩一半, 再 JPEG 压。
//      实测: 光掩码那 64 行(约 6 KB)在 115200 上就要 0.9 s (约 8 ms/行 = 正好是线速),
//      所以 P5.12 同时把控制台提到 921600, 否则"实时预览"无从谈起。
#define IMG_TX_ENABLE   1
#define IMG_TX_SCALE    2                        // 2x2 取平均 -> 320x240
#define IMG_TX_QUALITY  70
#define IMG_TX_W        (640 / IMG_TX_SCALE)
#define IMG_TX_H        (480 / IMG_TX_SCALE)

// P5.21: 94x24 裁剪块的 JPEG 输出缓冲上限。
//   原来这里写死成 IMG_W*IMG_H = 2256 字节, 注释还写着"一定小于这个数" —— 是错的:
//   实测 q90 编出来就有 2764 字节, 而 jpeg_enc_process() 不会因为 outbuf_size 小而收手,
//   多出来的 508 字节直接写到紧跟在后面的那些全局变量上 (g_tx_* / g_enc / g_tx_rgb ... 全被糊掉),
//   于是"切到模式 2 的第一帧"就 panic (LoadProhibited)。
//   JPEG 体积正常不会超过原始 RGB 大小, 这里按原始大小再留 4 KB 余量, 溢不出来。
#define IMG_TX_CROP_JPG_MAX  (IMG_W * IMG_H * 3 + 4096)


// P5.15/P5.16: 图片输出模式 —— 短按 BOOT 循环切换。
// P5.74: 档位重排, 0 改成"关", 另外三档顺延 (BOOT 短按的循环顺序 = 关->预览->绿框->输入块->关):
//   0 = 关 (不发图) —— **默认档**, 也是接真节点用的档: 节点只要文本, 一张 320x240 JPEG 十几 KB, 会把链路塞满
//   1 = 干净预览图 (320x240, 采数据/日常看画面)
//   2 = 预览图 + ROI 绿框 (专门用来看"框套得准不准")
//   3 = 模型输入块 (94x24) —— 就是模型真正吃到的那张小图, 逐像素一致, 直接当训练素材
// P5.68: 图片永远走**控制台口** (esp_rom_output_tx_one_char -> UART0/USB), 跟链路宏没关系 ——
//   所以它压根不占节点那条线。
// P5.74: 图片档位不再跟控制权挂钩 —— 电脑口在**两种模式**下都能设 AT+IMG (图片只走电脑那条口, 不占节点线),
//   所以正常模式下也能一边跑节点一边看画面。开机默认 0(关); 档位非 0 时每条 $PLATE 尾巴上带
//   ",img=<档位>" 当提醒, 免得"图还开着"只能靠自己记得。
//   want = 用户设的(AT+IMG / BOOT 短按 改的都是它), 生效 = 当前真正用的; 现在两者恒等,
//   留着两个变量只是为了以后要做"临时覆盖"时有地方下手。
static volatile int g_img_mode_want = 0;
static volatile int g_img_mode = 0;

/* P5.74: 生效档 = 用户设的档(跟控制权无关); 顺手把档位告诉链路层, 好让 $PLATE 带上 img= 提醒 */
static void img_mode_apply(void) {
    g_img_mode = g_img_mode_want;
    node_link_set_img_hint(g_img_mode);
}

// P5.31: 详细模式开关 (长按 BOOT 切换)。原来长按只是"详细一帧", 想连看几帧就得反复长按 ——
//   而"主色框 vs 死门槛框差几个像素"这种结论, 恰恰要连着好几帧才看得出稳不稳。
//   现在长按 = 开关: 开着的每一帧都走详细, 再长按一次关掉。
//   代价: 每帧多约 100 行日志 (候选表 + 覆盖率剖面), 921600 下约 90 ms。掩码那 60 行仍受 ROI_MASK_BUDGET 限制。
static volatile bool g_verbose_mode = false;

/*
 * P5.76: 日志两档 —— 简单模式(默认)把 ESP_LOG 整个关掉, 只留结果块 + 开机两行;
 *   详细模式(AT+LOG=1 / 长按 BOOT)把 plate 放开, 全套诊断回来。
 *   nodelink 那几行(镜像/权限/控制权)算链路诊断, 简单模式只留警告。
 *   注意 sdkconfig: CONFIG_LOG_DEFAULT_LEVEL=NONE 且 MAXIMUM=INFO —— 开机那堆(boot/esp_psram/
 *   cpu_start)在编译期就静音了, 但运行时还能靠这里把日志重新打开。
 */
static void apply_log_levels(void)
{
    if (g_verbose_mode) {
        esp_log_level_set("*", ESP_LOG_INFO);
    } else {
        esp_log_level_set("*", ESP_LOG_NONE);
        esp_log_level_set("nodelink", ESP_LOG_WARN);
    }
}

// P5.68: 节点侧触发 (AT+RUN) 与"只听触发"(AT+TRIG=1) —— 这两件事要碰模型句柄/摄像头状态,
//   那些都是 app_main 的局部量, 所以命令只置标记, 真正干活放在主循环里。
static volatile bool g_run_request = false;    // AT+RUN: 请主循环立刻拍一帧
static volatile bool g_trigger_only = true;     // 默认「只听触发」: 定时自动识别关着, 发 AT+TRIG=0 才恢复连续
// P5.34/P5.59: "长边收边"(按实测蓝色边界把取样框左端收窄) —— **P5.61 已整段删除**。
//   定论: 171 张原始帧按固件真流程复算, 收左端 76/171 = 44%, 不收 128/171 = 75%,
//   而且"收左端对"的 76 帧完全落在"不收对"的 128 帧里面 —— 0 帧有帮助、52 帧帮倒忙。
//   根因: P5.60 换了 b-r 判据之后定位框本来就贴着车牌, 蓝带量出来的"左端那一段不是蓝"其实就是
//   **省字自己**(省字笔画太密, 一行 9 个采样点里蓝点常常不到 4 个), 再按它收 = 把省字切掉,
//   真机表现就是 京 -> 皖/粤/沪 乱跳、或者整串少一个字。详见文档第四十五节。
static uint8_t g_crop_u8[IMG_W * IMG_H * 3] __attribute__((aligned(16)));  // 模式 2: 输入张量反量化回来的 RGB (JPEG 源序)。必须 16 字节对齐 —— esp_new_jpeg v0.6 起在 S3 上会检查编码器输入缓冲的对齐

// ===================== S5: 车牌缩略图 (94×24 二值位图, 282B) =====================
// 模型输入反量化 -> 灰度 -> 自适应阈值(σ=6 高斯局部背景, 亮于背景 +12 记白字)。
// 参数与 L1 预览脚本 _conv_preview.py 的"自适应"档一致(两张真牌实测可读)。
// 位序: 2256 比特行优先连续打包(每行 94 比特不断行), 每字节高位在前(bit7 = 第 0 个像素),
//       1 = 白字 0 = 黑底 —— 节点/网关只透传, App 端按同样规则点阵绘制。
// 产图时机: recognize_once 里模型输入填充处(必须在 run() 之前, 理由同 g_crop_u8);
//           不看 g_img_mode 档位, 每帧都算(94×24 高斯两趟 ≈ 1ms)。
static uint8_t g_thumb[IMG_W * IMG_H / 8];          // 94*24/8 = 282
static volatile bool g_thumbValid = false;          // 1=有图可取
static volatile uint16_t g_thumbNo = 0;             // 每出一张新图 ++, 节点透传给网关丢旧图残包

static void thumb_build(const int8_t *cp)
{
    static uint16_t kw[25];        // σ=6 高斯核, 半径 12, 定点 q16 (和 = 65536)
    static bool kinit = false;
    static uint8_t gray[IMG_W * IMG_H];
    static uint8_t bg[IMG_W * IMG_H];
    const int R = 12, W = IMG_W, H = IMG_H, N = IMG_W * IMG_H;

    if (!kinit) {                  // 核只算一次: exp(-i²/2σ²) 归一化后放大到 65536
        double sum = 0;
        for (int i = -R; i <= R; i++) sum += exp((double)-(i * i) / (2.0 * 6.0 * 6.0));
        uint32_t acc = 0;
        for (int i = -R; i <= R; i++) {
            double v = exp((double)-(i * i) / (2.0 * 6.0 * 6.0)) / sum * 65536.0;
            kw[i + R] = (uint16_t)(v + 0.5);
            acc += kw[i + R];
        }
        kw[R] = (uint16_t)(kw[R] + (65536u - acc));   // 抹平取整误差, 保证权重和恰为 65536
        kinit = true;
    }

    // 灰度: cp 是 B,G,R 序(与 g_crop_u8 填充同源), BT.601 定点
    for (int i = 0; i < N; i++) {
        const int b = g_inv_lut[(int)cp[i * 3 + 0] + 128];
        const int g = g_inv_lut[(int)cp[i * 3 + 1] + 128];
        const int r = g_inv_lut[(int)cp[i * 3 + 2] + 128];
        gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
    }

    // 水平高斯(边界钳制) -> 暂存
    for (int y = 0; y < H; y++) {
        const uint8_t *src = gray + y * W;
        uint8_t *dst = bg + y * W;
        for (int x = 0; x < W; x++) {
            uint32_t acc = 0;
            for (int d = -R; d <= R; d++) {
                int xx = x + d;
                if (xx < 0) xx = 0; else if (xx >= W) xx = W - 1;
                acc += (uint32_t)src[xx] * kw[d + R];
            }
            dst[x] = (uint8_t)(acc >> 16);
        }
    }
    // 垂直高斯 -> 局部背景
    static uint8_t tmp[IMG_W * IMG_H];
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            uint32_t acc = 0;
            for (int d = -R; d <= R; d++) {
                int yy = y + d;
                if (yy < 0) yy = 0; else if (yy >= H) yy = H - 1;
                acc += (uint32_t)bg[yy * W + x] * kw[d + R];
            }
            tmp[y * W + x] = (uint8_t)(acc >> 16);
        }
    }

    // 二值: 字比局部背景亮 12 以上 = 白字(1), 否则黑底(0), 行优先打包
    memset(g_thumb, 0, sizeof(g_thumb));
    for (int i = 0; i < N; i++) {
        if ((int)gray[i] > (int)tmp[i] + 12)
            g_thumb[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
    g_thumbValid = true;
    g_thumbNo++;
}

static uint8_t *g_tx_rgb = nullptr;              // RGB888 (16 字节对齐), PSRAM
static uint8_t *g_tx_jpg = nullptr;              // JPEG 输出缓冲, PSRAM
static uint8_t *g_prov_rgb = nullptr;            // P5.67: 省字 patch 的原色 RGB (PROV_W*PROV_H*3), 详细模式/低置信时发回串口
// P5.17: 硬件 JPEG 编码器只有一份 —— 以前预览图 (320x240) 和裁剪块 (94x24) 各开了一个句柄,
//   两个句柄抢同一份硬件: 用另一个尺寸编过之后, 再切回旧句柄编码就卡死在 jpeg_enc_process()
//   里出不来了 (表现: 按 BOOT 切完模式后毫无反应, 必须重启)。现在只留一个句柄,
//   配置变了就 close 再 open。
static jpeg_enc_handle_t g_enc = nullptr;
static int g_enc_w = 0, g_enc_h = 0, g_enc_q = 0;
// P5.20: 这里原来有个"一次性关掉图片输出"的开关 —— 一旦置上就再也不发图了, 只能重启单片机。
//   现在改成: 这一帧失败就只跳过这一帧, 下一帧照常重试; 失败原因 + 次数会打出来 (同一原因最多 3 s 报一次)。
static uint32_t g_tx_ok = 0;                     // 累计成功发出的帧数
static uint32_t g_tx_bytes = 0;                  // 累计发出的字节数
static uint32_t g_tx_fail = 0;                   // 连续失败次数
static const char *g_tx_fail_why = nullptr;      // 最近一次失败原因
static int64_t g_tx_warn_us = 0;                 // 上次打印失败原因的时刻 (给日志限流)

/** 保证编码器当前是按 (w,h,q) 打开的; 配置变了就换一个 (close 再 open) */
static bool img_tx_enc_ensure(int w, int h, int q) {
    if (g_enc != nullptr && g_enc_w == w && g_enc_h == h && g_enc_q == q) return true;
    if (g_enc != nullptr) {
        jpeg_enc_close(g_enc);
        g_enc = nullptr;
    }
    jpeg_enc_config_t cfg = {};                 // 逐字段赋值, 不用 DEFAULT_JPEG_ENC_CONFIG() (C++ 下那个宏有坑)
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;    // 编码器不吃 RGB565, 必须 RGB888
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = (uint8_t)q;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.task_enable = false;
    cfg.hfm_task_priority = 0;
    cfg.hfm_task_core = 0;
    if (jpeg_enc_open(&cfg, &g_enc) != JPEG_ERR_OK || g_enc == nullptr) {
        g_enc = nullptr;
        ESP_LOGW(TAG, "JPEG 编码器打开失败 (%dx%d q%d) -> 本帧不发图", w, h, q);
        return false;
    }
    g_enc_w = w;
    g_enc_h = h;
    g_enc_q = q;
    return true;
}

/** 这一帧的图成功发出去了: 记一笔账; 要是刚才是连续失败, 报一次"恢复" */
static void img_tx_note_ok(int len) {
    g_tx_ok++;
    g_tx_bytes += (uint32_t)len;
    if (g_tx_fail > 0) {
        ESP_LOGW(TAG, "发图已恢复 (刚才连续失败 %u 次, 原因: %s)", (unsigned)g_tx_fail,
                 g_tx_fail_why ? g_tx_fail_why : "?");
        g_tx_fail = 0;
        g_tx_fail_why = nullptr;
    }
    if (g_tx_ok % 30 == 0) {   // 每 30 帧报一次: 这就是"字节确实从单片机发出去过"的证据
        ESP_LOGI(TAG, "发图统计: 累计成功 %u 帧, 共 %u KB", (unsigned)g_tx_ok, (unsigned)(g_tx_bytes / 1024));
    }
}

/** 这一帧的图没发出去: 只跳过这一帧 (不再永久关闭图片输出), 同一原因最多 3 s 报一次, 免得刷屏 */
static void img_tx_note_fail(const char *why) {
    g_tx_fail++;
    g_tx_fail_why = why;
    const int64_t now = esp_timer_get_time();
    if (g_tx_fail == 1 || now - g_tx_warn_us > 3000000) {
        g_tx_warn_us = now;
        ESP_LOGW(TAG, "这一帧发图失败 (%u 次): %s", (unsigned)g_tx_fail, why);
    }
}

/**
 * 把"图片输出"这条链整体复位: 关掉编码器、清掉失败计数。
 * 按一下 BOOT 就会走这里 —— 万一以后又卡住, 按一下就能救回来, 不用再重启单片机。
 */
static void img_tx_reset(const char *why) {
    if (g_enc != nullptr) {
        jpeg_enc_close(g_enc);
        g_enc = nullptr;
    }
    g_enc_w = g_enc_h = g_enc_q = 0;
    g_tx_fail = 0;
    g_tx_fail_why = nullptr;
    ESP_LOGW(TAG, "图片输出已复位 (%s): 编码器已关闭, 下一帧重新打开", why);
}

/** zlib CRC-32 (与 Python binascii.crc32 完全一致) */
static uint32_t img_tx_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/** P5.12 关键: 二进制必须"原样"发, 不能用 fwrite(stdout)。
 *  踩过的坑: 控制台输出会把每个 '\n'(0x0A) 换成 "\r\n"(0x0D 0x0A) ——
 *  见 IDF 的 esp_rom_putc (esp_rom_sys.c: "if (c == '\n') tx('\r')")。
 *  JPEG 里 0x0A 非常常见(实测 4.3 KB 的图里 30 个左右), 于是助手收到的字节数变多、
 *  长度对不上, CRC32 必然校验失败 -> 整帧被丢掉(实测: 去掉多出来的 \r 后长度/CRC 立刻吻合)。
 *  esp_rom_output_tx_one_char 是 ROM 里"发一个字节"的底层函数, 不做任何转换, 正好用在这里。 */
static void img_tx_raw_write(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        esp_rom_output_tx_one_char(p[i]);
    }
}

/** 按 P5.12 协议发一帧 JPEG: "$IMG,<len>\r\n" + JPEG + CRC32(大端) + "$END\r\n" */
static void img_tx_send_jpeg(const uint8_t *jpg, int len) {
    // 写入过程里绝对不能有 LOG (任何一行文字都会把整帧搅坏): 先把 stdio 里积压的日志冲出去,
    // 保证帧头不会插在日志中间; 之后全部走"原样字节" (stdout 会把 0x0A 变成 0x0D 0x0A)。
    fflush(stdout);
    char hdr[32];
    const int hl = snprintf(hdr, sizeof(hdr), "$IMG,%d\r\n", len);
    const uint32_t crc = img_tx_crc32(jpg, (size_t)len);
    const uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
                           (uint8_t)(crc >> 8), (uint8_t)crc};
    img_tx_raw_write((const uint8_t *)hdr, (size_t)hl);
    img_tx_raw_write(jpg, (size_t)len);
    img_tx_raw_write(cb, 4);
    img_tx_raw_write((const uint8_t *)"$END\r\n", 6);
}

// P5.22: 框线宽度 (像素)。原来固定 1 px, 在图上细得看不清 —— 现在 4 px。
#define IMG_TX_BOX_THICK 4

/** 画一条线 (Bresenham), 只用来把 ROI 框画在预览图上 —— 纯绿, 一眼能看见。
 *  thick = 线宽: 每个像素点落一个 thick×thick 的实心方块 (框线用这个够了, 不用抗锯齿)。 */
static void img_tx_line(uint8_t *rgb, int w, int h, int x0, int y0, int x1, int y1, int thick) {
    if (thick < 1) thick = 1;
    const int half = thick / 2;
    const int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = -half; oy < thick - half; oy++) {
            for (int ox = -half; ox < thick - half; ox++) {
                const int px = x0 + ox, py = y0 + oy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                uint8_t *q = rgb + ((size_t)py * w + px) * 3;
                q[0] = 0; q[1] = 255; q[2] = 0;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/**
 * 把这一帧(缩一半, 可能画上 ROI 框)编码成 JPEG 发到串口。
 * box == nullptr 表示这一帧没定位到车牌 —— 图照样发, 正好让用户看见"相机到底看见了什么"。
 * P5.14: 连续帧一律传 nullptr(不画框) —— 那才是要存成训练素材的图, 绿线会被模型当成车牌的一部分。
 */
static void img_tx_send(const uint8_t *rgb565be, int w, int h, const roi_box_t *box) {
#if IMG_TX_ENABLE
    if (w != IMG_TX_W * IMG_TX_SCALE || h != IMG_TX_H * IMG_TX_SCALE) {
        // 只支持 640x480 这一档 (改了 framesize 就要同步改 IMG_TX_*)
        img_tx_note_fail("相机帧尺寸不是 640x480");
        return;
    }

    // 必须 16 字节对齐 (S3 上编码器按 128bit 读); 放 PSRAM, 不占内部 RAM。
    // P5.20: 两个缓冲分开补 —— 以前只要有一次没分配上, 图片输出就被永久关掉了。
    if (g_tx_rgb == nullptr) {
        g_tx_rgb = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)IMG_TX_W * IMG_TX_H * 3,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (g_tx_jpg == nullptr) {
        g_tx_jpg = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)IMG_TX_W * IMG_TX_H,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (g_tx_rgb == nullptr || g_tx_jpg == nullptr) {
        img_tx_note_fail("PSRAM 缓冲分配失败 (内存不够或者太碎)");
        return;
    }
    // ---- 1) RGB565(大端) -> RGB888, 顺便 2x2 平均降采样 (平均比抽点干净, 不会有摩尔纹) ----
    const int s = IMG_TX_SCALE, nn = s * s;
    for (int y = 0; y < IMG_TX_H; y++) {
        uint8_t *dst = g_tx_rgb + (size_t)y * IMG_TX_W * 3;
        for (int x = 0; x < IMG_TX_W; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < s; dy++) {
                const uint8_t *row = rgb565be + ((size_t)(y * s + dy) * w + x * s) * 2;
                for (int dx = 0; dx < s; dx++) {
                    const uint8_t b0 = row[dx * 2], b1 = row[dx * 2 + 1];
                    sr += b0 & 0xF8;                                            // 与 esp-dl 大端宏一致
                    sg += (((b0 & 0x07) << 5) | ((b1 & 0xE0) >> 3)) & 0xFF;
                    sb += (b1 & 0x1F) << 3;
                }
            }
            dst[x * 3 + 0] = (uint8_t)(sr / nn);   // R
            dst[x * 3 + 1] = (uint8_t)(sg / nn);   // G
            dst[x * 3 + 2] = (uint8_t)(sb / nn);   // B
        }
    }

    // ---- 2) 把 ROI 框画上去 (能摆正就画四角, 否则画外接框) ----
    if (box != nullptr) {
        const float inv = 1.0f / (float)IMG_TX_SCALE;
        int px[4], py[4];
        if (box->rot_ok) {
            for (int k = 0; k < 4; k++) {
                px[k] = (int)(box->qx[k] * inv + 0.5f);
                py[k] = (int)(box->qy[k] * inv + 0.5f);
            }
        } else {
            px[0] = (int)(box->x1 * inv); py[0] = (int)(box->y1 * inv);
            px[1] = (int)(box->x2 * inv); py[1] = (int)(box->y1 * inv);
            px[2] = (int)(box->x2 * inv); py[2] = (int)(box->y2 * inv);
            px[3] = (int)(box->x1 * inv); py[3] = (int)(box->y2 * inv);
        }
        for (int k = 0; k < 4; k++) {
            img_tx_line(g_tx_rgb, IMG_TX_W, IMG_TX_H, px[k], py[k], px[(k + 1) & 3], py[(k + 1) & 3], IMG_TX_BOX_THICK);
        }
    }

    // ---- 3) JPEG 编码 (全局单句柄, 见 img_tx_enc_ensure) ----
    if (!img_tx_enc_ensure(IMG_TX_W, IMG_TX_H, IMG_TX_QUALITY)) {
        img_tx_note_fail("JPEG 编码器打不开");
        return;
    }

    int out_len = 0;
    if (jpeg_enc_process(g_enc, g_tx_rgb, IMG_TX_W * IMG_TX_H * 3,
                         g_tx_jpg, IMG_TX_W * IMG_TX_H, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        img_tx_note_fail("JPEG 编码失败 (预览图 320x240)");
        return;
    }
    if (out_len > IMG_TX_W * IMG_TX_H) {   // 防御: 同上, 超了就不发
        img_tx_note_fail("JPEG 编出来超过缓冲 (预览图)");
        return;
    }

    img_tx_send_jpeg(g_tx_jpg, out_len);
    img_tx_note_ok(out_len);
#else
    (void)rgb565be; (void)w; (void)h; (void)box;
#endif
}

/** 模式 3: 把"模型输入块"发出去。图只有 94x24, 但它是训练素材的正品 —— 和推理输入逐像素一致 */
static void img_tx_send_crop(void) {
#if IMG_TX_ENABLE
    if (!img_tx_enc_ensure(IMG_W, IMG_H, 90)) {   // 训练素材, 压得轻一点 (q90)
        img_tx_note_fail("JPEG 编码器打不开 (94x24)");
        return;
    }

    // P5.21: 必须用足够大的缓冲 —— 见 IMG_TX_CROP_JPG_MAX 的说明 (写小了会把后面的全局变量糊掉)
    static uint8_t jpg[IMG_TX_CROP_JPG_MAX] __attribute__((aligned(16)));
    int out_len = 0;
    if (jpeg_enc_process(g_enc, g_crop_u8, IMG_W * IMG_H * 3,
                         jpg, sizeof(jpg), &out_len) != JPEG_ERR_OK || out_len <= 0) {
        img_tx_note_fail("JPEG 编码失败 (94x24 裁剪块)");
        return;
    }
    if (out_len > (int)sizeof(jpg)) {   // 防御: 真编超了就宁可不发, 也不能越界写坏内存
        img_tx_note_fail("JPEG 编出来超过缓冲 (94x24)");
        return;
    }
    img_tx_send_jpeg(jpg, out_len);
    img_tx_note_ok(out_len);
#endif
}
// P5.67: 把"省字复核模型真正吃到的那块 32x64 patch"发回串口 (最近邻放大), 便于肉眼/助手确认。
//   为什么要它: 复核置信塌下来的时候, 光看文字日志分不清"窗口没对准"和"模型不行";
//   把 patch 发回来一眼就能看出省字有没有落在窗口里、窗口是不是被背景带偏了。
static void img_tx_send_prov(int zoom) {
#if IMG_TX_ENABLE
    // P5.74: 发图统一只看图片输出档 (0=关就一张都不发); 不再跟控制权挂钩。
    //   这条是唯一绕过 g_img_mode 判断的发图路(省字复核 patch), 闸门在这儿补上。
    if (g_img_mode == 0) return;
    if (g_prov_rgb == nullptr || zoom < 1) return;
    const int bw = PROV_W * zoom, bh = PROV_H * zoom;
    static uint8_t *big = nullptr;
    static uint8_t *jpg = nullptr;
    if (big == nullptr) {
        big = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)bw * bh * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        jpg = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)bw * bh * 3 + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (big == nullptr || jpg == nullptr) {
        img_tx_note_fail("省字 patch 缓冲分配失败 (PSRAM)");
        return;
    }
    for (int y = 0; y < bh; y++) {
        const uint8_t *srow = g_prov_rgb + (size_t)(y / zoom) * PROV_W * 3;
        uint8_t *drow = big + (size_t)y * bw * 3;
        for (int x = 0; x < bw; x++) {
            const uint8_t *s = srow + (size_t)(x / zoom) * 3;
            drow[x * 3 + 0] = s[0];
            drow[x * 3 + 1] = s[1];
            drow[x * 3 + 2] = s[2];
        }
    }
    if (!img_tx_enc_ensure(bw, bh, 88)) {
        img_tx_note_fail("JPEG 编码器打不开 (省字 patch)");
        return;
    }
    int out_len = 0;
    if (jpeg_enc_process(g_enc, big, bw * bh * 3, jpg, (size_t)bw * bh * 3 + 4096, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        img_tx_note_fail("JPEG 编码失败 (省字 patch)");
        return;
    }
    img_tx_send_jpeg(jpg, out_len);
    img_tx_note_ok(out_len);
#else
    (void)zoom;
#endif
}

// P5.12: 精简日志 —— roi_locate 定位失败时把原因记在这里, 由 recognize_once 合成一行打印
static const char *g_roi_note = "(未知原因)";
static bool g_roi_fallback = false;   // P5.46: 本帧是不是靠"兜底档"(面积够大就收)才挑到框
static int g_frame_no = 0;

/** 是否是车牌颜色: 蓝/绿车牌的色相窗口 + 饱和度/亮度下限
 *  (OpenCV 8bit: H 0..179, S/V 0..255)
 *
 *  P5.17 把蓝色窗口从 [100,124] (即 200°~248°, 只有 48° 宽) 放宽到 180°~270° + S>=35.
 *  为什么: 绿牌窗口 70°~170° 有 100° 宽, 蓝牌却只有 48° —— 蓝牌那边本来就写窄了.
 *  实测 (97 张屏幕翻拍的蓝牌, 见文档第二十八章):
 *      蓝色窗口        牌内像素通过   牌外误报   能出框的比例
 *      [200°,248°]      30.8%        1.7%         19%     <- 改之前: 掩码是碎的/中空的
 *      [185°,265°]      61.7%        2.2%         75%
 *      [180°,270°]+S35  70.1%        2.8%         90%
 *  色相窗口太窄 => 掩码只剩零散碎片 => "有效填充率"过不了 60% 的门槛 => 整帧被丢掉,
 *  表现就是"明明车牌拍得很清楚, 却一直跳过推理". 放宽后牌外误报几乎没涨,
 *  因为真正区分"车牌"和"蓝东西"的是后面的形状/填充率门槛, 不是像素颜色. */
static inline bool roi_is_plate_color(int r, int g, int b) {
    const int v = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    const int m = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    if (v < ROI_MIN_VALUE) return false;       // P5.49: 亮度下限 46 -> 120 (原来是 V >= 46)
    // P5.60: 蓝牌先走这条 —— 只有比较和减法, 没有除法也没有色相, 所以既更稳也更快
    //   (旧判据要算 s = 255*delta/v 和整数色相, 那是"定位 156 ms"里的主要开销)。
    if ((b - r) > ROI_BLUE_BR_MIN && b > ROI_MIN_VALUE) return true;
    const int delta = v - m;
    if (delta == 0) return false;
    // P5.27 性能: 先做一个"不用除法"的饱和度粗筛。s = 255*delta/v < 35 等价于 255*delta < 35*v,
    //   而这种像素色相再合适也过不了 (蓝牌要 s>=35, 绿牌要 s>=43)。
    //   整帧 30 万像素里绝大多数是灰/白/黑/低饱和, 这一句让它们直接返回,
    //   省掉除法 + 取色相那一段 —— 那是"定位 156 ms"里的主要开销。
    if (255 * delta < 35 * v) return false;
    const int s = (255 * delta) / v;           // 饱和度
    int deg;
    if (v == r) {
        deg = 60 * (g - b) / delta;
        if (deg < 0) deg += 360;
    } else if (v == g) {
        deg = 60 * (b - r) / delta + 120;
    } else {
        deg = 60 * (r - g) / delta + 240;
    }
    // P5.60: 蓝牌已经在上面按 (b-r) 判过了, 走到这里说明这个像素"蓝得不明显" —— 不要。
    //   (旧规则 = 色相 180~270 且 s>=35, 屏摄时会把反光/屏幕泛蓝一起放进来。)
    if (deg >= 180 && deg <= 270) return false;
    if (deg >= 70 && deg <= 170) return s >= 43;    // 绿牌 (新能源), 未动
    return false;
}

/** 粗网格上的水平形态学: opp=0 膨胀(OR) / opp=1 腐蚀(AND); 结构元 4x1, 锚点居中(偏移 -2..+1) */
static void roi_morph_h(const uint8_t *in, uint8_t *out, int opp) {
    for (int y = 0; y < ROI_GRID_H; y++) {
        const uint8_t *row = in + (size_t)y * ROI_GRID_W;
        uint8_t *dst = out + (size_t)y * ROI_GRID_W;
        for (int x = 0; x < ROI_GRID_W; x++) {
            uint8_t acc = opp ? 1 : 0;
            for (int k = -2; k <= 1; k++) {
                const int xx = x + k;
                if (xx < 0 || xx >= ROI_GRID_W) continue;   // 出界不参与(等价 OpenCV 默认 border)
                if (opp) acc &= row[xx];
                else     acc |= row[xx];
            }
            dst[x] = acc;
        }
    }
}

static int roi_find_root(int16_t *parent, int i) {
    int r = i;
    while (parent[r] != r) r = parent[r];
    while (parent[i] != r) {
        const int nxt = parent[i];
        parent[i] = (int16_t)r;
        i = nxt;
    }
    return r;
}

/** 一个连通域(候选车牌)的原始外接框, 格坐标 */
typedef struct {
    int root;
    int x1, y1, x2, y2;
    int area;             // 连通域格数
} roi_slot_t;

/** PCA 拟合出的旋转矩形 (等价 cv2.minAreaRect) + 摆正用的四角 */
typedef struct {
    bool  ok;             // false = 拟合不合格, 调用方退回轴对齐裁剪
    float ratio;          // 旋转矩形的长宽比 (真车牌本体约 3.1~3.4)
    float fill;           // 旋转矩形内的填充率 (低 => 连通域是散块/被背景撑大)
    float deg;            // 相对水平方向的倾角, 单位度
    float uw, vh;         // 旋转矩形的边长, 单位格
    float qx[4], qy[4];   // 去边后的四角, 原图像素, 顺序 左上/右上/右下/左下
    // --- P5.22: 诊断字段 (只在详细模式/按 BOOT 时打出来, 连续模式不占任何开销) ---
    const char *why;      // 拟合结论: "通过" 或具体死在哪一条
    int np;               // 这个连通域里"车牌色格"的个数
} roi_fit_t;

/**
 * 在某个连通域上做 PCA 拟合旋转矩形, 并给出摆正用的四角。
 * 训练集是 warpPerspective 摆正过的 (LPRNet_Pytorch/extract_ccpd.py:76-90), 板端必须做同样的事,
 * 否则斜着拍的车牌会被轴对齐外接框斜着拉伸 (文档 13.5 / 14.6 / 15.3)。
 * 点集 = "属于该连通域 且 该格本身真有车牌色" 的格心 (排除形态学膨胀出来的晕圈)。
 */
static bool roi_fit_rotated(const uint8_t *cell, const uint8_t *bin, int16_t *parent,
                            const roi_slot_t *sl, int w, int h, roi_fit_t *fit) {
    fit->ok = false;
    fit->ratio = fit->fill = fit->deg = fit->uw = fit->vh = 0.0f;
    fit->why = "未完成拟合";
    fit->np = 0;
    int np = 0;
    double sx = 0, sy = 0;
    for (int y = sl->y1; y <= sl->y2; y++) {
        for (int x = sl->x1; x <= sl->x2; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i] || cell[i] < ROI_CELL_NEED) continue;
            if (roi_find_root(parent, i) != sl->root) continue;
            np++;
            sx += x;
            sy += y;
        }
    }
    fit->np = np;
    if (np < 30) { fit->why = "车牌色格数太少 (<30 格)"; return false; }   // 30 格 = 480 px^2, 再小拟合没有意义
    const double mx = sx / np, my = sy / np;
    double cxx = 0, cyy = 0, cxy = 0;
    for (int y = sl->y1; y <= sl->y2; y++) {
        for (int x = sl->x1; x <= sl->x2; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i] || cell[i] < ROI_CELL_NEED) continue;
            if (roi_find_root(parent, i) != sl->root) continue;
            const double dx = x - mx, dy = y - my;
            cxx += dx * dx;
            cyy += dy * dy;
            cxy += dx * dy;
        }
    }
    cxx /= np; cyy /= np; cxy /= np;
    // 主轴 = 协方差矩阵最大特征值的方向: 对矩形来说就是长边方向
    const double theta = 0.5 * atan2(2.0 * cxy, cxx - cyy);
    const double ux = cos(theta), uy = sin(theta);
    double umin = 1e9, umax = -1e9, vmin = 1e9, vmax = -1e9;
    for (int y = sl->y1; y <= sl->y2; y++) {
        for (int x = sl->x1; x <= sl->x2; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i] || cell[i] < ROI_CELL_NEED) continue;
            if (roi_find_root(parent, i) != sl->root) continue;
            const double dx = x - mx, dy = y - my;
            const double u = dx * ux + dy * uy;
            const double v = -dx * uy + dy * ux;
            if (u < umin) umin = u;
            if (u > umax) umax = u;
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
    }
    const double uw = umax - umin, vh = vmax - vmin;
    if (uw < 6.0 || vh < 3.0) { fit->why = "旋转矩形太小 (要 >=24x12 像素)"; return false; }   // 至少 24x12 像素才值得摆正
    const double ratio = uw / vh;
    const double fill = (double)np / ((uw + 1.0) * (vh + 1.0));
    fit->ratio = (float)ratio;
    fit->fill = (float)fill;
    fit->deg = (float)(theta * 57.29577951308232);
    fit->uw = (float)uw;
    fit->vh = (float)vh;
    if (ratio < 1.2 || ratio > 8.0) { fit->why = "旋转长宽比越界 (要 1.2~8.0)"; return false; }
    if (fill < ROI_FIT_MIN_FILL) { fit->why = "旋转矩形填充不足 (<35%)"; return false; }   // P5.46: 0.45 -> 0.35; 被反光打散的斜牌也能走摆正路径 (严格档选框时仍按 60% 填充筛)
    // 去边: 与轴对齐分支同一套比例 (左右 1% / 上下 3%), 在 u/v 上就是收缩区间
    const double um0 = umin + uw * ROI_TRIM_X, um1 = umax - uw * ROI_TRIM_X;
    const double vm0 = vmin + vh * ROI_TRIM_Y, vm1 = vmax - vh * ROI_TRIM_Y;
    if (um1 <= um0 || vm1 <= vm0) { fit->why = "去边后区间为空"; return false; }
    const double uu[4] = {um0, um1, um1, um0};
    const double vv[4] = {vm0, vm0, vm1, vm1};
    double cxs[4], cys[4];
    for (int k = 0; k < 4; k++) {
        const double gx = mx + uu[k] * ux - vv[k] * uy;
        const double gy = my + uu[k] * uy + vv[k] * ux;
        cxs[k] = (gx + 0.5) * ROI_GRID_STEP;
        cys[k] = (gy + 0.5) * ROI_GRID_STEP;
        if (cxs[k] < 0) cxs[k] = 0;
        if (cys[k] < 0) cys[k] = 0;
        if (cxs[k] > w - 1) cxs[k] = w - 1;
        if (cys[k] > h - 1) cys[k] = h - 1;
    }
    // 从 (x+y) 最小的角开始; 两个邻角里更靠右的那个是"右上"
    int st = 0;
    for (int k = 1; k < 4; k++) {
        if (cxs[k] + cys[k] < cxs[st] + cys[st]) st = k;
    }
    int nxt = (st + 1) % 4;
    if (cxs[(st + 3) % 4] > cxs[nxt]) nxt = (st + 3) % 4;
    const int ord[4] = {st, nxt, (st + 2) % 4, (nxt + 2) % 4};
    for (int k = 0; k < 4; k++) {
        fit->qx[k] = (float)cxs[ord[k]];
        fit->qy[k] = (float)cys[ord[k]];
    }
    fit->ok = true;
    fit->why = "通过";
    return true;
}

/**
 * 把 1/4 粗网格上的"车牌色格"画成 80x60 的 ASCII 掩码, 并把选中的框描上去。
 *   '.'=不是车牌色   ','=4 格里 1~2 格是   '#'=4 格里 3~4 格是   '+'=选中的框 (有旋转四角就描四边形)
 * 用途: 一眼看清"车牌色"落在画面哪里、选中的框有没有套上去 —— 这比任何猜测都直接。
 * 80x60 不小, 所以只在开机头几次会话 + 定位失败/结果不像车牌时打印(见调用点)。
 */
static void roi_dump_mask(const uint8_t *cell, int need, const roi_box_t *box) {
    // 要描的边: 有旋转四角就描四边形(最能看出"框有没有套住车牌"), 否则描轴对齐外框。
    // 边框位图只在第一次调用时分配 (PSRAM), 失败就只画"车牌色"本身。
    static uint8_t *border = nullptr;
    if (border == nullptr) {
        border = (uint8_t *)heap_caps_malloc((size_t)ROI_GRID_N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (border) {
        memset(border, 0, (size_t)ROI_GRID_N);
        if (box && box->rot_ok) {
            const int seg = 64;   // 每条边 64 个采样点 (格子只有 4 px)
            for (int e = 0; e < 4; e++) {
                const int e2 = (e + 1) % 4;
                const float x0 = box->qx[e] / ROI_GRID_STEP, y0 = box->qy[e] / ROI_GRID_STEP;
                const float x1 = box->qx[e2] / ROI_GRID_STEP, y1 = box->qy[e2] / ROI_GRID_STEP;
                for (int s = 0; s <= seg; s++) {
                    const float t = (float)s / (float)seg;
                    int gx = (int)(x0 + (x1 - x0) * t);
                    int gy = (int)(y0 + (y1 - y0) * t);
                    if (gx < 0) gx = 0;
                    if (gy < 0) gy = 0;
                    if (gx > ROI_GRID_W - 1) gx = ROI_GRID_W - 1;
                    if (gy > ROI_GRID_H - 1) gy = ROI_GRID_H - 1;
                    border[(size_t)gy * ROI_GRID_W + gx] = 1;
                }
            }
        } else if (box) {
            const int bx1 = box->x1 / ROI_GRID_STEP, by1 = box->y1 / ROI_GRID_STEP;
            const int bx2 = (box->x2 - 1) / ROI_GRID_STEP, by2 = (box->y2 - 1) / ROI_GRID_STEP;
            for (int x = bx1; x <= bx2; x++) {
                if (x < 0 || x >= ROI_GRID_W) continue;
                if (by1 >= 0 && by1 < ROI_GRID_H) border[(size_t)by1 * ROI_GRID_W + x] = 1;
                if (by2 >= 0 && by2 < ROI_GRID_H) border[(size_t)by2 * ROI_GRID_W + x] = 1;
            }
            for (int y = by1; y <= by2; y++) {
                if (y < 0 || y >= ROI_GRID_H) continue;
                if (bx1 >= 0 && bx1 < ROI_GRID_W) border[(size_t)y * ROI_GRID_W + bx1] = 1;
                if (bx2 >= 0 && bx2 < ROI_GRID_W) border[(size_t)y * ROI_GRID_W + bx2] = 1;
            }
        }
    }
    std::string art = "\n";
    for (int gy = 0; gy < ROI_GRID_H; gy += 2) {
        for (int gx = 0; gx < ROI_GRID_W; gx += 2) {
            int hit = 0, on_border = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    const int x = gx + dx, y = gy + dy;
                    if (x >= ROI_GRID_W || y >= ROI_GRID_H) continue;
                    if (cell[(size_t)y * ROI_GRID_W + x] >= need) hit++;
                    if (border && border[(size_t)y * ROI_GRID_W + x]) on_border = 1;
                }
            }
            if (on_border) art += '+';
            else if (hit >= 3) art += '#';
            else if (hit) art += ',';
            else art += '.';
        }
        art += '\n';
    }
    ESP_LOGI(TAG, "车牌色掩码 (1 字符 = 8x8 像素, '#'=车牌色, ','=零星, '+'=选中的框/四角):%s", art.c_str());
}

/**
 * 在整帧里找车牌 ROI (整条链路的入口)。找到返回 true 并填好 box:
 *   box.x1/y1/x2/y2      = 轴对齐外接框 (已去边; 日志/退回/掩码都用它)
 *   box.rot_ok/qx/qy     = PCA 拟合的旋转矩形四角 (能拟合时才有, 用来把车牌摆正)
 * 工作缓冲区首次调用时一次性分配 (PSRAM), 后续复用。
 * verbose=false 时不打印 80x60 掩码 (实时连续模式跑得快, 需要掩码时按 BOOT 即可)。
 */
static bool roi_locate(const uint8_t *rgb565be, int w, int h, roi_box_t *box, bool verbose) {
    static int16_t *parent = nullptr;
    static uint8_t *cell = nullptr;   // 每格车牌色像素数 0..16
    static uint8_t *bin = nullptr;
    static uint8_t *tmp = nullptr;

    if (w != ROI_GRID_W * ROI_GRID_STEP || h != ROI_GRID_H * ROI_GRID_STEP) {
        ESP_LOGE(TAG, "ROI 只支持 %dx%d, 当前帧 %dx%d", ROI_GRID_W * ROI_GRID_STEP,
                 ROI_GRID_H * ROI_GRID_STEP, w, h);
        return false;
    }
    if (parent == nullptr) {
        const size_t need = (size_t)ROI_GRID_N;
        parent = (int16_t *)heap_caps_malloc(need * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        cell = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bin = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        tmp = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        roi_cell_snap = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (parent == nullptr || cell == nullptr || bin == nullptr || tmp == nullptr || roi_cell_snap == nullptr) {
        ESP_LOGE(TAG, "ROI 工作缓冲区分配失败");
        return false;
    }

    // ---- 1) 逐像素判定车牌色, 累加到 1/4 粗网格; 顺便统计整帧颜色 ----
    memset(cell, 0, (size_t)ROI_GRID_N);
    long sum_r = 0, sum_g = 0, sum_b = 0;
    long color_px = 0;
    long over_px = 0, dark_px = 0;   // P5.24: 整帧过曝/死黑像素数 (顺手统计, 几乎不花时间)
    for (int y = 0; y < h; y++) {
        const uint8_t *row = rgb565be + (size_t)y * w * 2;
        uint8_t *crow = cell + (size_t)(y / ROI_GRID_STEP) * ROI_GRID_W;
        for (int x = 0; x < w; x++) {
            const uint8_t b0 = row[x * 2], b1 = row[x * 2 + 1];
            const int r = b0 & 0xF8;                                       // 与 esp-dl 大端宏一致
            const int g = (((b0 & 0x07) << 5) | ((b1 & 0xE0) >> 3)) & 0xFF;
            const int b = (b1 & 0x1F) << 3;
            sum_r += r;
            sum_g += g;
            sum_b += b;
            // P5.24: 曝光体检。过曝会把车牌色的饱和度洗掉(掩码变碎、填充率过不了 60% 门槛),
            //   欠曝则蓝底发黑压不出色相 —— 这两种都能靠"最亮通道"的分布直接看出来。
            const int mx3 = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
            if (mx3 >= 250) over_px++;
            else if (mx3 <= 5) dark_px++;
            if (roi_is_plate_color(r, g, b)) {
                crow[x / ROI_GRID_STEP]++;
                color_px++;
            }
        }
    }
    // 这两行是"最可信"的前置指标 (在 run() 之前、且直接来自相机帧):
    //   三通道均值几乎相等 + 车牌色占比很低  => 画面本身没被车牌色主导;
    //   均值里 B 明显最大                      => 解码出来的通道顺序没搞反。
    const float inv_px = 1.0f / (float)(w * h);
    if (verbose) {   // P5.12: 连续模式一行都不打, 这些只在按 BOOT 时看
        ESP_LOGI(TAG, "整帧均值(像素 0..255): R=%.0f G=%.0f B=%.0f | 车牌色像素 %ld/%d = %.1f%%",
                 (float)sum_r * inv_px, (float)sum_g * inv_px, (float)sum_b * inv_px,
                 color_px, w * h, 100.0f * (float)color_px * inv_px);
        const float over_p = 100.0f * (float)over_px * inv_px;
        const float dark_p = 100.0f * (float)dark_px * inv_px;
        ESP_LOGI(TAG, "整帧曝光: 过曝(最亮通道>=250) %.1f%% | 死黑(<=5) %.1f%% | %s",
                 over_p, dark_p,
                 (over_p > 8.0f) ? "明显过曝 —— 车牌色会被洗淡, 避开强光/反光再试"
                                 : ((dark_p > 25.0f) ? "整体偏暗 —— 蓝底可能压不出色相" : "正常"));
    }
    int color_cells = 0;
    // 快照一份车牌色格给 recognize_once 用: 本函数返回后 cell 仍是本帧数据,
    // 但它是静态缓冲、下次调用就被覆盖, 所以导出到 roi_cell_snap (独立 PSRAM 块)。
    memcpy(roi_cell_snap, cell, (size_t)ROI_GRID_N);

    for (int i = 0; i < ROI_GRID_N; i++) {
        bin[i] = (cell[i] >= ROI_CELL_NEED) ? 1 : 0;
        color_cells += bin[i];
    }
    if (color_cells == 0) {
        g_roi_note = "画面里没有一块车牌色 —— 车牌太小/太远/太暗, 或者偏色";
        if (verbose) {
            ESP_LOGW(TAG, "整帧没有一个是车牌色 -> 没看到车牌(或颜色通道/白平衡有问题)");
            roi_dump_mask(cell, ROI_CELL_NEED, nullptr);
        }
        return false;
    }

    // ---- 2) 闭运算 = 膨胀+腐蚀, 开运算 = 腐蚀+膨胀 (复刻原脚本 MORPH_CLOSE / MORPH_OPEN) ----
    roi_morph_h(bin, tmp, 0);
    roi_morph_h(tmp, bin, 1);
    roi_morph_h(bin, tmp, 1);
    roi_morph_h(tmp, bin, 0);

    // ---- 3) 连通域 (union-find, 4 邻域) ----
    for (int i = 0; i < ROI_GRID_N; i++) parent[i] = bin[i] ? (int16_t)i : (int16_t)-1;
    for (int y = 0; y < ROI_GRID_H; y++) {
        for (int x = 0; x < ROI_GRID_W; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i]) continue;
            if (x > 0 && bin[i - 1]) {
                const int a = roi_find_root(parent, i), b = roi_find_root(parent, i - 1);
                if (a != b) parent[a] = (int16_t)b;
            }
            if (y > 0 && bin[i - ROI_GRID_W]) {
                const int a = roi_find_root(parent, i), b = roi_find_root(parent, i - ROI_GRID_W);
                if (a != b) parent[a] = (int16_t)b;
            }
        }
    }

    // ---- 4) 按根节点归并外接框, 取面积最大的 ROI_MAX_SLOTS 个当候选, 再用原脚本的 score 挑最好的 ----
    // P5.33: 候选表不再"先到先得", 改成"按连通域面积取前 32 名"。
    //   背景 (2026-09-30, 27 帧实拍 豫A1890P): 24 帧的连通域数都撞到 32 的上限, 而"车牌色格"里
    //   一大半是画面各处的零散蓝点 —— 先到先得会把先扫到的小噪点塞满候选表(按扫描顺序, 也就是
    //   画面左上角优先), 真正的车牌反而进不来, 表现成"有车牌色但没找到合格的矩形"(27 帧里 11 帧)。
    //   连通域越大越可能是车牌, 所以按面积留前 32 名。
    //   连通域总数 <= 32 时, 候选集合与排列顺序都和旧实现逐位一致 => 本来就认对的帧不受影响。
    static int16_t root_ord[ROI_GRID_N];    // 根节点 -> 首次出现的先后 (19200*2B, 大数组放静态区)
    static int16_t root_area[ROI_GRID_N];   // 根节点 -> 连通域格数
    int nroot = 0;
    memset(root_ord, 0xFF, sizeof(root_ord));    // -1 = 这个根还没出现过
    memset(root_area, 0, sizeof(root_area));
    for (int y = 0; y < ROI_GRID_H; y++) {
        for (int x = 0; x < ROI_GRID_W; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i]) continue;
            const int root = roi_find_root(parent, i);
            if (root_ord[root] < 0) root_ord[root] = (int16_t)nroot++;
            root_area[root]++;
        }
    }
    int sel_root[ROI_MAX_SLOTS], sel_area[ROI_MAX_SLOTS];
    int nslot = 0;
    if (nroot <= ROI_MAX_SLOTS) {
        for (int r = 0; r < ROI_GRID_N; r++) {          // 全部装得下: 保持旧的"首次出现"顺序
            if (root_ord[r] < 0) continue;
            sel_root[root_ord[r]] = r;
            sel_area[root_ord[r]] = root_area[r];
        }
        nslot = nroot;
    } else {
        for (int r = 0; r < ROI_GRID_N; r++) {          // 面积降序插入; 满了就把最小的挤出去
            if (root_ord[r] < 0) continue;
            const int area = root_area[r];
            if (nslot >= ROI_MAX_SLOTS && area <= sel_area[nslot - 1]) continue;
            int p = (nslot < ROI_MAX_SLOTS) ? nslot : ROI_MAX_SLOTS - 1;
            while (p > 0 && sel_area[p - 1] < area) {
                sel_area[p] = sel_area[p - 1];
                sel_root[p] = sel_root[p - 1];
                p--;
            }
            sel_area[p] = area;
            sel_root[p] = r;
            if (nslot < ROI_MAX_SLOTS) nslot++;
        }
    }
    roi_slot_t slots[ROI_MAX_SLOTS];
    memset(root_ord, 0xFF, sizeof(root_ord));           // 复用成 "根 -> 候选下标"
    for (int k = 0; k < nslot; k++) {
        root_ord[sel_root[k]] = (int16_t)k;
        slots[k].root = sel_root[k];
        slots[k].x1 = ROI_GRID_W; slots[k].y1 = ROI_GRID_H;
        slots[k].x2 = -1;         slots[k].y2 = -1;
        slots[k].area = 0;
    }
    for (int y = 0; y < ROI_GRID_H; y++) {              // 第二趟: 只算这 nslot 个的外接框
        for (int x = 0; x < ROI_GRID_W; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i]) continue;
            const int k = root_ord[roi_find_root(parent, i)];
            if (k < 0) continue;
            if (x < slots[k].x1) slots[k].x1 = x;
            if (x > slots[k].x2) slots[k].x2 = x;
            if (y < slots[k].y1) slots[k].y1 = y;
            if (y > slots[k].y2) slots[k].y2 = y;
            slots[k].area++;
        }
    }

    const int min_cells = (int)((float)ROI_GRID_N * ROI_MIN_AREA_RATIO);
    int dens_rej = 0;   // 过了宽高比、但被填充率门槛淘汰的候选数 (只用于日志)
    // 先给每个候选算好 (旋转拟合 / 宽高比 / 框内密度 / 得分), 再挑一个。
    // ⚠ 宽高比过滤 (2.0~6.5) 必须加在"旋转矩形"的比例上, 拿不到才退回轴对齐 bbox:
    //   原脚本的 minAreaRect 给的就是旋转矩形; 轴对齐 bbox 会被倾斜"撑高"
    //   (离线实验: 300x80 的车牌斜 20° => bbox 247x142 = 1.74, 照搬过滤条件会判成"没有 ROI")。
    // 密度 = 连通域格数 / 外接框格数: 真车牌是一整块蓝底(白字挖掉一点), 接近 1; 散块很低。
    float c_ratio[ROI_MAX_SLOTS], c_axis_ratio[ROI_MAX_SLOTS];
    float c_dens[ROI_MAX_SLOTS], c_denseff[ROI_MAX_SLOTS], c_score[ROI_MAX_SLOTS];
    float c_area_pct[ROI_MAX_SLOTS];   // P5.56: 每个候选的占屏 %, 兜底档的占屏下限要用它
    bool c_ok[ROI_MAX_SLOTS], c_okr[ROI_MAX_SLOTS], c_capped[ROI_MAX_SLOTS];   // P5.49: 被占屏上限一刀切掉的
    const char *c_why[ROI_MAX_SLOTS];   // P5.22: 每个候选"为什么通过/被淘汰" (详细模式打出来)
    static roi_fit_t c_fit[ROI_MAX_SLOTS];   // 每个候选的旋转拟合结果 (大数组放静态区)
    for (int k = 0; k < ROI_MAX_SLOTS; k++) {
        c_ratio[k] = c_axis_ratio[k] = c_dens[k] = c_denseff[k] = c_score[k] = 0.0f;
        c_area_pct[k] = 0.0f;
        c_ok[k] = c_okr[k] = c_capped[k] = false;
        c_why[k] = "未评估";
        c_fit[k].ok = false;
    }
    for (int k = 0; k < nslot; k++) {
        // P5.45: 面积/比例/填充**无条件全算出来** —— 原来"连通域太小"就 continue 了, 这样松档
        //   拿不到小连通域的比例/填充, 松档等于白设。判定本身仍是下面这三道严格门槛。
        const float bw = (float)(slots[k].x2 - slots[k].x1 + 1);
        const float bh = (float)(slots[k].y2 - slots[k].y1 + 1);
        c_axis_ratio[k] = bw / bh;
        roi_fit_rotated(cell, bin, parent, &slots[k], w, h, &c_fit[k]);
        const float ratio = c_fit[k].ok ? c_fit[k].ratio : c_axis_ratio[k];
        c_ratio[k] = ratio;
        c_dens[k] = (float)slots[k].area / (bw * bh);
        // 有效填充率: 摆正得了就用旋转矩形的填充率 (不随倾角掉), 否则退回轴对齐外接框密度
        c_denseff[k] = c_fit[k].ok ? c_fit[k].fill : c_dens[k];
        c_score[k] = (float)slots[k].area / (1.0f + fabsf(ratio - ROI_RATIO_IDEAL));
        // P5.49: 占屏上限。屏幕反光/背景与车牌连成一片时, 连通域会涨到占屏 50%~90%(真车牌本体 <=~32%),
        //   这时严格档的比例/填充本来就会把它否掉, 但兜底档"只看面积"会把它整个收下 -> 框=一整屏。
        //   所以在源头一刀切: 超上限的候选, 严格档和兜底档一律不要, 该帧就按"没找到"处理。
        const float area_pct_k = c_fit[k].ok
                                 ? (100.0f * c_fit[k].uw * c_fit[k].vh / (float)ROI_GRID_N)
                                 : (100.0f * bw * bh / (float)ROI_GRID_N);
        c_area_pct[k] = area_pct_k;   // P5.56: 存下来给兜底档用
        if (area_pct_k > ROI_MAX_AREA_PCT) { c_capped[k] = true; c_why[k] = "占屏过大(疑似反光/背景连成一片)"; continue; }
        // P5.58: 占屏下限, 严格档也拦 —— 见 ROI_MIN_AREA_PCT 的说明
        if (area_pct_k < ROI_MIN_AREA_PCT) { c_why[k] = "占屏太小(不可能是车牌)"; continue; }
        if (slots[k].area < min_cells) { c_why[k] = "连通域太小"; continue; }
        if (ratio < ROI_RATIO_LO || ratio > ROI_RATIO_HI) {
            // 倾斜的牌照最常死在这一条: 旋转拟合没成功 -> 退回轴对齐外接框 -> 外接框被倾斜撑胖 -> 比例不过
            c_why[k] = c_fit[k].ok ? "旋转宽高比越界" : "宽高比越界(拟合失败, 退回轴对齐)";
            continue;
        }
        c_okr[k] = true;
        if (c_denseff[k] < ROI_MIN_FILL) { dens_rej++; c_why[k] = "有效填充不足"; continue; }   // 太稀 => 不是车牌, 宁可不猜
        c_ok[k] = true;
        c_why[k] = "通过";
    }

    // P5.22: 详细模式把**每一个**候选都打出来 (不只前 5 名), 并写清"为什么被淘汰" ——
    //   倾斜的牌照最常死在"旋转拟合没成功 -> 退回轴对齐 -> 比例/填充不过"这条路上, 必须能看见。
    if (verbose) {
        int shown = 0;
        for (int k = 0; k < nslot && k < ROI_MAX_SLOTS; k++) {
            if (c_why[k] == nullptr || strcmp(c_why[k], "未评估") == 0) continue;
            shown++;
            if (c_fit[k].ok) {
                ESP_LOGI(TAG, "候选%d: 像素x[%d,%d) y[%d,%d) 格%dx%d 色格%d | 旋转比例 %.2f 倾角 %+.1f° 旋转填充 %.0f%% | 轴对齐比例 %.2f 外接框密度 %.0f%% | 得分 %.0f -> %s",
                         k, slots[k].x1 * ROI_GRID_STEP, (slots[k].x2 + 1) * ROI_GRID_STEP,
                         slots[k].y1 * ROI_GRID_STEP, (slots[k].y2 + 1) * ROI_GRID_STEP,
                         slots[k].x2 - slots[k].x1 + 1, slots[k].y2 - slots[k].y1 + 1, slots[k].area,
                         c_fit[k].ratio, c_fit[k].deg, c_fit[k].fill * 100.0f,
                         c_axis_ratio[k], c_dens[k] * 100.0f, c_score[k], c_why[k]);
            } else {
                ESP_LOGI(TAG, "候选%d: 像素x[%d,%d) y[%d,%d) 格%dx%d 色格%d | 旋转拟合失败: %s | 轴对齐比例 %.2f 外接框密度 %.0f%% | 得分 %.0f -> %s",
                         k, slots[k].x1 * ROI_GRID_STEP, (slots[k].x2 + 1) * ROI_GRID_STEP,
                         slots[k].y1 * ROI_GRID_STEP, (slots[k].y2 + 1) * ROI_GRID_STEP,
                         slots[k].x2 - slots[k].x1 + 1, slots[k].y2 - slots[k].y1 + 1, slots[k].area,
                         c_fit[k].why ? c_fit[k].why : "?",
                         c_axis_ratio[k], c_dens[k] * 100.0f, c_score[k], c_why[k]);
            }
        }
        ESP_LOGI(TAG, "候选小结: 列出 %d 个 / 候选表 %d 个 (全画面车牌色连通域 %d 个%s); 门槛 = 面积>=%d格 比例%.1f~%.1f 有效填充>=%.0f%%",
                 shown, nslot, nroot, (nroot > ROI_MAX_SLOTS) ? ", 已按面积挤掉更小的" : "",
                 min_cells, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);
    }
    int best_k = -1;
    for (int k = 0; k < nslot; k++) {
        if (!c_ok[k]) continue;
        if (best_k < 0 || c_score[k] > c_score[best_k]) best_k = k;
    }
    // ---- P5.46: 严格档一个合格候选都没有 -> 兜底档: 面积够大就收, 比例/填充一概不看 ----
    //   为什么: 屏幕反光把蓝底打散后, 连通域不是"车牌形状", 严格档的比例/填充会把它一起否掉,
    //   而它确实是车牌。形状当不了裁判 —— 改由模型 + 车牌语法当裁判 (见 recognize_once 的结果闸门)。
    bool fallback_pass = false;
    if (best_k < 0) {
        float best2 = -1.0f;
        int fb_too_small = 0;
        for (int k = 0; k < nslot; k++) {
            if (slots[k].area < min_cells) continue;   // 只留"面积够大"这一条底线(与严格档同数), 不放开小碎点
            if (c_capped[k]) continue;                 // P5.49: 占屏过大的一律不收 —— 否则"兜底"会把整屏背景收下来
            if (c_area_pct[k] < ROI_MIN_AREA_PCT) { fb_too_small++; continue; }   // P5.56: 占屏太小也不收
            if (c_score[k] > best2) { best2 = c_score[k]; best_k = k; }
        }
        if (best_k < 0 && fb_too_small > 0) {
            ESP_LOGW(TAG, "兜底档也放弃: %d 个候选占屏 < %.1f%% (太小, 认不准, 宁可不报)", fb_too_small, ROI_MIN_AREA_PCT);
        }
        fallback_pass = (best_k >= 0);
        if (fallback_pass) {
            ESP_LOGW(TAG, "严格门槛挑不出候选 -> 兜底收下 候选%d (色格%d 比例%.2f 填充%.0f%%; 形状不看) [兜底]",
                     best_k, slots[best_k].area, c_ratio[best_k], c_denseff[best_k] * 100.0f);
        }
    }
    g_roi_fallback = fallback_pass;
    if (verbose && fallback_pass) {
        ESP_LOGW(TAG, "本帧走兜底档: 面积>=%d格 且 占屏>=%.1f%%, 比例/填充**不看** (严格档是 比例%.1f~%.1f 填充>=%.0f%%)",
                 min_cells, ROI_MIN_AREA_PCT, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);
    }
    // P5.24: 最佳与第二名的得分差距 —— 差距很小说明"这一帧选得勉强", 一旦选错, 后面再准也没用。
    if (verbose && best_k >= 0 && !fallback_pass) {   // P5.45: 走松档时不比第二名(c_ok 全是 false, 比了会误报)
        int k2 = -1;
        for (int k = 0; k < nslot; k++) {
            if (!c_ok[k] || k == best_k) continue;
            if (k2 < 0 || c_score[k] > c_score[k2]) k2 = k;
        }
        if (k2 >= 0) {
            const float s1 = c_score[best_k], s2 = c_score[k2];
            const float lead = (s1 > 0.0f) ? (100.0f * (s1 - s2) / s1) : 0.0f;
            ESP_LOGI(TAG, "选中 候选%d (得分 %.0f); 第二名 候选%d (得分 %.0f, 落后 %.0f%%)%s",
                     best_k, s1, k2, s2, lead,
                     (lead < 15.0f) ? " —— 两个候选很接近, 有选错的风险" : "");
        } else {
            ESP_LOGI(TAG, "选中 候选%d (得分 %.0f) —— 没有第二个合格候选", best_k, c_score[best_k]);
        }
    }
    if (verbose && dens_rej > 0) {
        ESP_LOGW(TAG, "形状像车牌但有效填充 < %.0f%% 被淘汰的候选: %d 个 (真车牌一般 >=65%%, 散块噪点 <=55%%)",
                 ROI_MIN_FILL * 100.0f, dens_rej);
    }
    if (best_k < 0) {
        // P5.45: 把"最接近门槛的那个候选"直接写进跳过原因 —— 差在面积、比例还是填充, 一行就看见,
        //   下次调门槛不用再翻候选表 (实测这一列才是"要不要再放松"的唯一依据)。
        int miss_k = -1;
        int capped_n = 0;   // P5.49: 被占屏上限切掉的块数 (只用于日志)
        for (int k = 0; k < nslot; k++) {
            if (c_ok[k]) continue;
            if (c_capped[k]) { capped_n++; continue; }   // P5.49: 被占屏上限切掉的不算"最接近"
            if (miss_k < 0 || slots[k].area > slots[miss_k].area) miss_k = k;
        }
        static char miss_note[256];
        if (miss_k < 0 && capped_n > 0) {
            // P5.49: 有车牌色、也有连通域, 只是每一块都太大 —— 和"太散"是两回事, 必须分开报
            snprintf(miss_note, sizeof(miss_note),
                     "有车牌色, 但每一块都太大(占屏 >%.0f%%) —— 像是反光/背景与车牌连成一片: %d 块被占屏上限切掉",
                     ROI_MAX_AREA_PCT, capped_n);
        } else if (miss_k < 0) {
            snprintf(miss_note, sizeof(miss_note), "有车牌色, 但一个车牌色连通域都没有 —— 太散 (车牌色格 %d 个)", color_cells);
        } else {
            snprintf(miss_note, sizeof(miss_note),
                     "有车牌色, 但没有一块像车牌的长方形 —— 最接近的候选: 色格%d 比例%.2f 填充%.0f%% | 淘汰于[%s] | 门槛 色格>=%d 比例%.1f~%.1f 填充>=%.0f%%",
                     slots[miss_k].area, c_ratio[miss_k], c_denseff[miss_k] * 100.0f, c_why[miss_k],
                     min_cells, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);
        }
        g_roi_note = miss_note;
        if (verbose) {
            ESP_LOGW(TAG, "有车牌色但没找到合格的矩形 (车牌色格数 %d/%d, 其中填充不足淘汰 %d 个) —— 可能太小/倾斜, 或蓝色被反光洗白",
                     color_cells, ROI_GRID_N, dens_rej);
            roi_dump_mask(cell, ROI_CELL_NEED, nullptr);
        }
        return false;
    }

    // ---- 4.5) 贴到画面边缘的框 (P5.48) ----
    // 历史: P5.30~P5.46 = "贴边一律拒绝" (实测 2026-09-28: 贴边的 25 轮里只有 1 轮读对);
    //       P5.47 = "先换一个不贴边的替补, 没有才拒绝"。
    // P5.48 从现场日志 (2026-10-01 白天, 屏幕反光) 拿到两条新证据:
    //   1) 得分最高的候选贴边, 是因为**车牌和反光/背景连成了一片**, 不是车牌真出画面;
    //      P5.47 的替补往往更烂 (实测换成 色格300 比例15.86 的一条细缝), 换了更糟。
    //   2) 唯一读对的两帧 (#133 542x212 占屏38.6% / #140) 都是"框比车牌大一圈"也照样读对,
    //      说明模型吃得下带背景的框。
    // 所以: **兜底档贴边照用** (形状本来就不看), 结果交给"结果闸门"判;
    //       严格档(形状像车牌)贴边仍旧"先换替补, 没有才拒绝" —— 那种才是真被画面切了。
    {
        const bool touch_l = (slots[best_k].x1 == 0);
        const bool touch_r = ((slots[best_k].x2 + 1) * ROI_GRID_STEP >= w);
        const bool touch_t = (slots[best_k].y1 == 0);
        const bool touch_b = ((slots[best_k].y2 + 1) * ROI_GRID_STEP >= h);
        if (touch_l || touch_r || touch_t || touch_b) {
            char edges[16] = "";
            if (touch_l) strcat(edges, "左");
            if (touch_r) strcat(edges, "右");
            if (touch_t) strcat(edges, "上");
            if (touch_b) strcat(edges, "下");
            if (fallback_pass) {
                ESP_LOGW(TAG, "兜底档: 候选贴到画面%s边缘也照用 (形似反光/背景与车牌连成一片; 老固件会整帧丢掉) —— 结果交给闸门判",
                         edges);
            } else {
                int alt = -1;
                float alt_score = -1.0f;
                for (int k = 0; k < nslot; k++) {
                    if (k == best_k) continue;
                    if (!c_ok[k]) continue;   // 严格档的替补必须也是"形状像车牌"的合格候选
                    if (slots[k].x1 == 0) continue;
                    if ((slots[k].x2 + 1) * ROI_GRID_STEP >= w) continue;
                    if (slots[k].y1 == 0) continue;
                    if ((slots[k].y2 + 1) * ROI_GRID_STEP >= h) continue;
                    if (c_score[k] > alt_score) { alt_score = c_score[k]; alt = k; }
                }
                if (alt >= 0) {
                    ESP_LOGW(TAG, "选中的候选%d 贴到画面边缘 -> 改选不贴边的候选%d (色格%d 比例%.2f 填充%.0f%%)",
                             best_k, alt, slots[alt].area, c_ratio[alt], c_denseff[alt] * 100.0f);
                    best_k = alt;
                } else {
                    // P5.60: 原来这里是"整帧丢掉, 让用户把车牌移进画面"。
                    //   依据 (171 张原始帧复算): 判据修好之后, 贴边的那 9 帧里有 6 帧其实是**对的** ——
                    //   整帧丢掉是纯亏, 而且现场没法次次都把牌摆在画面中间。
                    //   真被画面切掉一半的那种, 后面的结果闸门(7~8 位 + 省字开头)仍会把乱码挡掉。
                    ESP_LOGW(TAG, "选中的框贴到画面%s边缘, 没有不贴边的替补 -> 照用 (老固件在这里整帧丢掉; 结果交给闸门判)",
                             edges);
                }
            }
        }
    }

    // ---- 5) 定框: 轴对齐 bbox 始终填好(日志/退回/掩码都用它), 能摆正时再补上四角 ----
    const float bw_c = (float)(slots[best_k].x2 - slots[best_k].x1 + 1);
    const float bh_c = (float)(slots[best_k].y2 - slots[best_k].y1 + 1);
    box->x1 = slots[best_k].x1 * ROI_GRID_STEP;
    box->y1 = slots[best_k].y1 * ROI_GRID_STEP;
    box->x2 = (slots[best_k].x2 + 1) * ROI_GRID_STEP;
    box->y2 = (slots[best_k].y2 + 1) * ROI_GRID_STEP;
    box->ratio = c_ratio[best_k];   // 能旋转拟合时是旋转矩形的比例, 否则是轴对齐外接框的
    box->area_pct = 100.0f * (bw_c * bh_c) / (float)ROI_GRID_N;
    box->score = c_score[best_k];
    box->density = c_denseff[best_k];   // 与候选过滤同一口径 (有效填充率)
    box->rot_ok = c_fit[best_k].ok;
    box->rot_ratio = c_fit[best_k].ratio;
    box->rot_fill = c_fit[best_k].fill;
    box->rot_deg = c_fit[best_k].deg;
    box->rot_why = c_fit[best_k].why;   // P5.24: 失败原因也带出去, 供"自动诊断"那一行用
    for (int k = 0; k < 4; k++) {
        box->qx[k] = c_fit[best_k].qx[k];
        box->qy[k] = c_fit[best_k].qy[k];
    }
    if (box->rot_ok) {   // 占屏比例也算旋转矩形的, 不然日志里会偏大
        box->area_pct = 100.0f * c_fit[best_k].uw * c_fit[best_k].vh / (float)ROI_GRID_N;
    }

    // ---- 5.5) P5.22 倾斜诊断 (只在详细模式/按 BOOT 时打) ----
    //   一眼看出"倾斜的牌照到底死在哪一环": 旋转拟合没过(退化成外接框) 还是 拟合过了但模型不认。
    if (verbose) {
        const roi_fit_t *f = &c_fit[best_k];
        ESP_LOGI(TAG, "倾斜诊断: 选中候选%d | 倾角 %+.1f° | 旋转拟合 %s (%s) 旋转比例 %.2f 旋转填充 %.0f%% | 轴对齐外接框 %dx%d 比例 %.2f | 本帧走 = %s",
                 best_k, f->deg, f->ok ? "成功" : "失败", f->why ? f->why : "?",
                 f->ratio, f->fill * 100.0f,
                 slots[best_k].x2 - slots[best_k].x1 + 1, slots[best_k].y2 - slots[best_k].y1 + 1,
                 c_axis_ratio[best_k],
                 f->ok ? "摆正(四边形采样)" : "轴对齐裁剪(退化路径)");
        if (f->ok) {
            // P5.24: 把四角坐标也打出来 —— 只看比例/填充率分不清"框很规整"和"框被背景撑成一条斜长条"。
            //   四边形面积 / 轴对齐外接框面积: 接近 1 说明框正(外接框几乎贴着四边形);
            //   明显小于 1 (比如 0.6) 说明外接框被"斜"撑得很大 —— 这种框去硬裁必然混进大量背景。
            const float *cx = f->qx;
            const float *cy = f->qy;
            const double qa = 0.5 * fabs((double)cx[0] * cy[1] - (double)cx[1] * cy[0]
                                         + (double)cx[1] * cy[2] - (double)cx[2] * cy[1]
                                         + (double)cx[2] * cy[3] - (double)cx[3] * cy[2]
                                         + (double)cx[3] * cy[0] - (double)cx[0] * cy[3]);
            const float bw_px = (float)(slots[best_k].x2 - slots[best_k].x1 + 1) * ROI_GRID_STEP;
            const float bh_px = (float)(slots[best_k].y2 - slots[best_k].y1 + 1) * ROI_GRID_STEP;
            ESP_LOGI(TAG, "四角(左上/右上/右下/左下, 原图像素): (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) | 四边形/外接框面积 = %.2f",
                     cx[0], cy[0], cx[1], cy[1], cx[2], cy[2], cx[3], cy[3],
                     (bw_px * bh_px > 0.0f) ? (qa / (double)(bw_px * bh_px)) : 0.0);
            ESP_LOGI(TAG, "四角边长: 长边 %.0f px 短边 %.0f px 比例 %.2f | 旋转矩形的长宽比 %.2f (真车牌约 3.1~3.4)",
                     f->uw * ROI_GRID_STEP, f->vh * ROI_GRID_STEP,
                     (f->vh > 0.0f) ? (f->uw / f->vh) : 0.0f, f->ratio);
        } else {
            ESP_LOGI(TAG, "四角: 无 —— 旋转拟合失败, 这一帧只能用轴对齐外接框硬裁 (斜牌这样裁必糊)");
        }
        if (!f->ok && fabsf(f->deg) >= 8.0f) {
            ESP_LOGW(TAG, "!! 牌照明显倾斜 (%.1f°) 但旋转拟合没成功 -> 只能用外接框裁, 框里混进背景、字符被压扁, 大概率认错", f->deg);
        }
        if (f->ok && fabsf(f->deg) >= 8.0f) {
            ESP_LOGW(TAG, "   倾斜 %.1f° 已走摆正路径 —— 若这一帧结果仍是乱码, 说明问题在模型(没见过这么大的倾角), 不在定位", f->deg);
        }
    }

    // ---- 6) 去边 (auto_crop_predict.py 的 左右 1% / 上下 3%) ----
    // 旋转分支的去边已经在 roi_fit_rotated 里按 u/v 做完了; 这里只处理轴对齐分支。
    const int bw = box->x2 - box->x1, bh = box->y2 - box->y1;
    const int mx = (int)((float)bw * ROI_TRIM_X);
    const int my = (int)((float)bh * ROI_TRIM_Y);
    box->x1 += mx; box->x2 -= mx;
    box->y1 += my; box->y2 -= my;
    if (!box->rot_ok && (box->x2 - box->x1 < 12 || box->y2 - box->y1 < 6)) {
        g_roi_note = "找到的框太小 —— 车牌离得太远";
        if (verbose) ESP_LOGW(TAG, "车牌框太小 (%dx%d), 放弃", box->x2 - box->x1, box->y2 - box->y1);
        return false;
    }
    if (box->x1 < 0) box->x1 = 0;
    if (box->y1 < 0) box->y1 = 0;
    if (box->x2 > w) box->x2 = w;
    if (box->y2 > h) box->y2 = h;

    // 开机头几次把掩码打出来(带框), 之后只在定位失败时打, 免得刷屏
    static int mask_left = ROI_MASK_BUDGET;
    if (verbose && mask_left > 0) {
        mask_left--;
        roi_dump_mask(cell, ROI_CELL_NEED, box);
    }
    return true;
}

/**
 * 单点双线性采样 RGB565(大端) 帧, 输出 8bit BGR。
 * 解码宏与 esp-dl 的 DL_IMAGE_BIG_ENDIAN_RGB565_* 逐位一致 (见文件头「RGB565 字节序」)。
 */
static void sample_rgb565_bilinear(const uint8_t *frame, int w, int h, float fx, float fy,
                                   int *out_b, int *out_g, int *out_r) {
    if (fx < 0) fx = 0;
    if (fx > (float)(w - 1)) fx = (float)(w - 1);
    if (fy < 0) fy = 0;
    if (fy > (float)(h - 1)) fy = (float)(h - 1);
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = (x0 + 1 < w) ? (x0 + 1) : x0;
    const int y1 = (y0 + 1 < h) ? (y0 + 1) : y0;
    const float ax = fx - (float)x0, ay = fy - (float)y0;
    const int xs[4] = {x0, x1, x0, x1};
    const int ys[4] = {y0, y0, y1, y1};
    const float wt[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    float acc[3] = {0, 0, 0};   // B, G, R
    for (int k = 0; k < 4; k++) {
        const uint8_t *q = frame + ((size_t)ys[k] * w + xs[k]) * 2;
        const int r = q[0] & 0xF8;
        const int g = (((q[0] & 0x07) << 5) | ((q[1] & 0xE0) >> 3)) & 0xFF;
        const int b = (q[1] & 0x1F) << 3;
        acc[0] += wt[k] * b;
        acc[1] += wt[k] * g;
        acc[2] += wt[k] * r;
    }
    int v[3];
    for (int c = 0; c < 3; c++) {
        v[c] = (int)(acc[c] + 0.5f);
        if (v[c] < 0) v[c] = 0;
        if (v[c] > 255) v[c] = 255;
    }
    *out_b = v[0];
    *out_g = v[1];
    *out_r = v[2];
}

/**
 * 按四个角点(左上/右上/右下/左下)把源帧摆正并缩放到 94x24 的 int8 输入张量。
 * 这一步顶替 dl::image::resize 的"轴对齐裁剪": 框是斜的时, 只有它能像训练链路那样把车牌
 * 摆正 (LPRNet_Pytorch/extract_ccpd.py:76-90 就是 warpPerspective), 否则字符会被斜着拉伸。
 * 归一化仍走同一张 LUT, 张量通道序仍是 BGR (与 dl_image_color.hpp 的 RGB_SWAP 分支一致)。
 *
 * P5.5: 抗锯齿(面积平均)。训练链路是 cv2.resize(默认 INTER_LINEAR) —— 缩小时它会对源像素
 *   做面积平均; 而"每个输出像素只取一个双线性点"是点采样: 一块 304x108 的车牌缩到 94x24 是
 *   3.2x4.5 倍抽稀, 白字笔画(源图里只有 2~6 px 宽)会随采样相位时有时无。现场表现就是
 *   同一块牌相邻两轮结果在跳、F 这种细笔画丢字。
 *   现在按"输出像素在源图上的足迹长度"决定子采样数 (1..SS_MAX) 再取平均 => 与训练端一致。
 *   足迹 < 1 像素(远处的牌) 时 nx=ny=1, 与改前的单点采样逐字节等价, 不会把图弄糊。
 *
 * P5.10: 面积平均只把图"抹平", 不解决"拍白了"。产出 94x24 的 BGR 之后, 交给
 *   crop_photometric_norm() 做一次整块的光度归一化(对比度+饱和度一起提, 色相不变),
 *   再用 LUT 量化成 int8 —— 所以归一化是在 BGR(0..255) 上做的, 不是在 int8 上做的。
 */
// P5.10: 上限从 4 提到 6。现场日志里 面积平均 一直是 4x4 —— 是"顶格"而不是"刚好":
//   框高 146px/24 行 = 竖直方向一个输出像素要吃 6.1 个源像素, 取 4 个就会跳着采样(残下摩尔纹),
//   收边后约 5.1, 仍然顶格。提到 6 以后竖直方向才真正平均开。代价: 预处理 88ms -> 约 110ms。
#define SS_MAX 6

// P5.9 收边: 沿"短边方向"(v) 找蓝色区域真正的上下边界。
//   拟合出来的框是"蓝格(PCA 外接范围) + 固定 3% 去边", 短边方向现场总是偏胖
//   (剖面: 最上段 1/9、最下段 3/9 是车牌色, 中间 5~8/9; 长短边比中位 2.61 vs 真车牌 3.14)
//   => 字符被竖直压扁。这里用覆盖率实测把两端收回来, 收到哪就只用哪一段。
#define ROI_VTRIM_N 24          // 沿 v 扫 24 行 (每行 4.2% 高)
#define ROI_VTRIM_UN 9          // 每行沿 u 取 9 点
#define ROI_VTRIM_MAX 0.25f     // 单端最多收 25%; 超过就认为"边界找错了", 这一端不收
#define ROI_VTRIM_MIN_KEEP 0.60f// 两端收完至少留 60% 高度, 否则整块不收
// P5.61: 这里原来还有 `ROI_UTRIM_LEFT_MAX` / `ROI_UTRIM_BAND_MIN` 两个常数, 配合同期的 roi_probe_utrim()
//   做"长边收边"(只收左端)。**已整段删除** —— 复算证明它 0 帧有帮助、52 帧帮倒忙(切省字),
//   根因见文档第四十五节。下面保留的只有"短边收边"(P5.9 起的 ROI_VTRIM_*), 那个是有效的。

// P5.10 裁剪块光度归一化 —— 在量化成 int8 之前, 把"屏摄/欠曝导致的发白"拉回来。
// 为什么需要: 百度 OCR 拿到的是整张原始 JPEG, 端侧模型拿到的是我们裁出来的 94x24 int8 小块,
//   信息量差两个数量级 —— 能自己补的功课只有"让这块图更清楚、对比度更强"。
// 做法: 以裁剪块自身的三通道均值为中心, 整块乘同一个比例 s:
//     v' = clamp(mean_c + (v - mean_c) * s)     c = B/G/R
//   同一个 s 同时放大"对比度"和"饱和度", 色相不变。
//   注意不能逐通道各自拉伸(那是 auto-levels): 那会把发白的蓝底三通道都拉到同一个暗值, 蓝底变灰,
//   而模型是在深蓝底上训练的 —— 色相必须保住。
// s 由亮度 p5/p95 定: 一块正常蓝牌(蓝底亮度约 76 + 白字 245)跨度约 170 => s≈1.2, 几乎不动;
//   现场屏摄那种发白的牌跨度只有 40~70 => s 顶到 3.0, 蓝底被推回深蓝、白字推到近白。
// 标定: CCPD 那种正常裁剪(蓝底亮度约 77 + 白字 245)跨度约 168 => s≈1.19 (轻微);
//   现场屏摄反推的蓝底(55,140,204)跨度约 123 => s≈1.63 (明显拉一把)。
//   若下一批日志显示"好图也被拉过头", 把 200 改回 168 即可让正常裁剪严格不动。
#define CROP_NORM_TARGET_SPREAD 200.0f   // 期望亮度跨度(p95-p5); 越大拉得越狠
#define CROP_NORM_S_MAX 3.0f             // 放大倍数上限
#define CROP_NORM_MIN_SPREAD 12          // 跨度小于它就别动(纯色块/噪声, 拉它没意义)

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

static uint8_t g_crop_rgb[IMG_W * IMG_H * 3];   // 裁剪块(摆正+面积平均后)暂存: B G R 交错
static int g_luma_hist[256];
/** P5.24: 裁剪块质量指标 —— 在光度归一化那一趟顺手算出来, 用来分开"图糊/过曝"和"模型不认识"。
 *  sharp = 平均 |Laplacian|(灰度): 白字笔画越利落值越大; 糊掉的图基本 < 5。
 *  valid = 本帧是否真的走过摆正路径 (轴对齐回退不经过这里, 那时这些数字是上一帧的, 不能信)。 */
typedef struct {
    int   valid;
    int   p_lo, p_hi, spread;    // 亮度 p5 / p95 / 跨度 (归一化前的原图, 0..255)
    float norm_s;                // 实际用上的对比度倍数
    int   over_pct, dark_pct;    // 过曝(最亮通道>=250) / 死黑(<=5) 的百分比
    float sharp;                 // 平均 |Laplacian|
    int   mb, mg, mr;            // 三通道均值 (0..255)
} crop_quality_t;
static crop_quality_t g_crop_q;

/** 裁剪块单像素灰度 (与亮度直方图同一套系数: 0.299R + 0.587G + 0.114B) */
static inline int crop_luma(const uint8_t *p, int i) {
    return (77 * p[i * 3 + 2] + 150 * p[i * 3 + 1] + 29 * p[i * 3 + 0]) >> 8;
}

static void crop_photometric_norm(int8_t *dst, const int8_t *lut, float *out_s) {
    const int NP = IMG_W * IMG_H;
    long sb = 0, sg = 0, sr = 0;
    memset(g_luma_hist, 0, sizeof(g_luma_hist));
    for (int i = 0; i < NP; i++) {
        const int b = g_crop_rgb[i * 3 + 0];
        const int g = g_crop_rgb[i * 3 + 1];
        const int r = g_crop_rgb[i * 3 + 2];
        sb += b; sg += g; sr += r;
        g_luma_hist[(77 * r + 150 * g + 29 * b) >> 8]++;   // 0.299R + 0.587G + 0.114B
    }
    const int n_lo = (NP * 5) / 100, n_hi = (NP * 95) / 100;
    int cum = 0, p_lo = 0, p_hi = 255;
    bool got_lo = false, got_hi = false;
    for (int i = 0; i < 256; i++) {
        cum += g_luma_hist[i];
        if (!got_lo && cum >= n_lo) { p_lo = i; got_lo = true; }
        if (!got_hi && cum >= n_hi) { p_hi = i; got_hi = true; break; }
    }
    const int spread = p_hi - p_lo;
    float s = 1.0f;
    if (spread >= CROP_NORM_MIN_SPREAD) {
        s = CROP_NORM_TARGET_SPREAD / (float)spread;
        if (s < 1.0f) s = 1.0f;                 // 只拉不压: 本来就清楚的就别动
        if (s > CROP_NORM_S_MAX) s = CROP_NORM_S_MAX;
    }
    const float mb = (float)sb / (float)NP, mg = (float)sg / (float)NP, mr = (float)sr / (float)NP;
    for (int i = 0; i < NP; i++) {
        int b = g_crop_rgb[i * 3 + 0], g = g_crop_rgb[i * 3 + 1], r = g_crop_rgb[i * 3 + 2];
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
        int8_t *o = dst + (size_t)i * 3;
        o[0] = lut[b];
        o[1] = lut[256 + g];
        o[2] = lut[512 + r];
    }
    // P5.24: 质量指标 (只能在这里算 —— g_crop_rgb 就是把这张图, 出了这个函数就被下一帧覆盖)。
    //   纯诊断, 不参与任何判断。
    {
        long over = 0, dark = 0, lap = 0;
        int lapn = 0;
        for (int i = 0; i < NP; i++) {
            const int b = g_crop_rgb[i * 3 + 0];
            const int g = g_crop_rgb[i * 3 + 1];
            const int r = g_crop_rgb[i * 3 + 2];
            int mx = (r > g) ? r : g;
            if (b > mx) mx = b;
            if (mx >= 250) over++;
            else if (mx <= 5) dark++;
        }
        for (int y = 1; y + 1 < IMG_H; y++) {
            for (int x = 1; x + 1 < IMG_W; x++) {
                const int i = y * IMG_W + x;
                const int d = 4 * crop_luma(g_crop_rgb, i)
                              - crop_luma(g_crop_rgb, i - 1) - crop_luma(g_crop_rgb, i + 1)
                              - crop_luma(g_crop_rgb, i - IMG_W) - crop_luma(g_crop_rgb, i + IMG_W);
                lap += (d < 0) ? -d : d;
                lapn++;
            }
        }
        g_crop_q.valid = 1;
        g_crop_q.p_lo = p_lo;
        g_crop_q.p_hi = p_hi;
        g_crop_q.spread = spread;
        g_crop_q.norm_s = s;
        g_crop_q.over_pct = (int)(100.0f * (float)over / (float)NP + 0.5f);
        g_crop_q.dark_pct = (int)(100.0f * (float)dark / (float)NP + 0.5f);
        g_crop_q.sharp = lapn ? ((float)lap / (float)lapn) : 0.0f;
        g_crop_q.mb = (int)(mb + 0.5f);
        g_crop_q.mg = (int)(mg + 0.5f);
        g_crop_q.mr = (int)(mr + 0.5f);
    }
    if (out_s) *out_s = s;
}

/** 双线性四边形求值: (u,v) in [0,1]^2 -> 源图像素坐标。u 沿宽(左->右), v 沿高(上->下) */
static void quad_eval(const float *qx, const float *qy, float u, float v, float *ox, float *oy) {
    const float w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = u * v, w3 = (1 - u) * v;
    *ox = w0 * qx[0] + w1 * qx[1] + w2 * qx[2] + w3 * qx[3];
    *oy = w0 * qy[0] + w1 * qy[1] + w2 * qy[2] + w3 * qy[3];
}

static void sample_quad_to_input(const uint8_t *frame, int w, int h,
                                 const float *qx, const float *qy,
                                 float u_lo, float u_hi,      // P5.61: 固定 [0,1] (长边收边已删, 形参保留)
                                 float v_lo, float v_hi,      // P5.9: 短边方向的取样区间 (收边后)
                                 int8_t *dst, const int8_t *lut,
                                 int *out_nx, int *out_ny, float *out_norm_s) {
    // P5.13/P5.32: u/v 都往外多取一点留白 —— quad_eval 是双线性外插, 等于把四边形按同心放大。
    // P5.32: 长边两端可以不一样; P5.40 起左右都由 g_pad_u_l / g_pad_u_r 运行时可切(见 k_pad_profiles)。
    // P5.61: u 恒为 [0,1] (长边收边已删), 所以 span_u_raw 恒为 1。
    const float span_v = v_hi - v_lo;
    const float span_u_raw = u_hi - u_lo;
    const float span_u = span_u_raw * (1.0f + g_pad_u_l + g_pad_u_r);
    const float u_base = u_lo - g_pad_u_l * span_u_raw;
    const float v_base = v_lo - ROI_PAD_V * span_v;
    const float du = span_u / (float)IMG_W;
    const float dv = (span_v * (1.0f + 2.0f * ROI_PAD_V)) / (float)IMG_H;
    int sum_nx = 0, sum_ny = 0, ncell = 0;
    for (int y = 0; y < IMG_H; y++) {
        const float v0 = v_base + (float)y * dv;
        for (int x = 0; x < IMG_W; x++) {
            const float u0 = u_base + (float)x * du;
            // 单元的三条边: 左下角由"左上+宽向+高向"隐含给出(平行四边形足够准, 还省一次求值)
            float ax, ay, bx, by, cx, cy;
            quad_eval(qx, qy, u0, v0, &ax, &ay);            // 单元左上角(源图坐标)
            quad_eval(qx, qy, u0 + du, v0, &bx, &by);       // 右上角
            quad_eval(qx, qy, u0, v0 + dv, &cx, &cy);       // 左下角
            const float ex = bx - ax, ey = by - ay;         // 宽向矢量(单位: 源像素)
            const float fx = cx - ax, fy = cy - ay;         // 高向矢量(单位: 源像素)
            // P5.13: 采样区间外扩了, 每个输出像素覆盖的源面积也等比变大, 不补这一下就欠采样(走样).
            // P5.32: 长边两端的留白不再对称, 所以两个方向的倍率也要分开算。
            const float scale_pad_u = span_u;
            const float scale_pad_v = 1.0f + 2.0f * ROI_PAD_V;
            const float len_x = sqrtf(ex * ex + ey * ey) * scale_pad_u;   // = 一个输出像素覆盖多少个源像素
            const float len_y = sqrtf(fx * fx + fy * fy) * scale_pad_v;
            int nx = (int)(len_x + 0.5f);
            int ny = (int)(len_y + 0.5f);
            if (nx < 1) nx = 1; else if (nx > SS_MAX) nx = SS_MAX;
            if (ny < 1) ny = 1; else if (ny > SS_MAX) ny = SS_MAX;
            sum_nx += nx; sum_ny += ny; ncell++;
            float acc[3] = {0, 0, 0};   // B, G, R
            for (int j = 0; j < ny; j++) {
                const float v = v0 + (j + 0.5f) * dv / (float)ny;
                for (int i = 0; i < nx; i++) {
                    const float u = u0 + (i + 0.5f) * du / (float)nx;
                    float sx, sy;
                    quad_eval(qx, qy, u, v, &sx, &sy);
                    int b, g, r;
                    sample_rgb565_bilinear(frame, w, h, sx, sy, &b, &g, &r);
                    acc[0] += (float)b; acc[1] += (float)g; acc[2] += (float)r;
                }
            }
            const float inv = 1.0f / (float)(nx * ny);
            int bb = (int)(acc[0] * inv + 0.5f);
            int gg = (int)(acc[1] * inv + 0.5f);
            int rr = (int)(acc[2] * inv + 0.5f);
            if (bb > 255) bb = 255; else if (bb < 0) bb = 0;
            if (gg > 255) gg = 255; else if (gg < 0) gg = 0;
            if (rr > 255) rr = 255; else if (rr < 0) rr = 0;
            uint8_t *o = g_crop_rgb + ((size_t)y * IMG_W + x) * 3;   // P5.10: 先落到暂存, 再整块归一化
            o[0] = (uint8_t)bb;
            o[1] = (uint8_t)gg;
            o[2] = (uint8_t)rr;
        }
    }
    if (out_nx) *out_nx = ncell > 0 ? (sum_nx + ncell / 2) / ncell : 1;
    if (out_ny) *out_ny = ncell > 0 ? (sum_ny + ncell / 2) / ncell : 1;
    crop_photometric_norm(dst, lut, out_norm_s);   // P5.10: 对比度/饱和度归一化 -> int8
}

/**
 * P5.67: 找"省字窗口该从哪开始" —— 沿车牌中线扫出蓝面真正的左边缘, 返回它的 u 值。
 *   为什么不信框的 u=0 边: 兜底框的左边缘可能把一圈背景(反光/泛蓝)一起框进来, 于是整扇省字窗被右推;
 *   PC 归因实验里"窗右推 20%~40% 个窗宽"正是唯一能把省字置信从中位 94% 打到 39% 的劣化。
 *   三条中线各扫一遍取中位数 —— 省字笔画密, 单条线可能整条压在白字上扫不到车牌色。
 *   返回 < 0 = 三条线都没扫到车牌色 (调用方退回老做法 u=0)。
 */
static float prov_find_u_left(const uint8_t *frame, int w, int h, const float *qx, const float *qy) {
    float uu[3] = {0.0f, 0.0f, 0.0f};
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const float v = 0.30f + 0.20f * (float)k;      // 三条中线: v = 0.30 / 0.50 / 0.70
        for (int i = 0; i <= PROV_SCAN_N; i++) {
            const float u = (float)i / (float)PROV_SCAN_N * PROV_SCAN_UMax;
            float sx, sy;
            quad_eval(qx, qy, u, v, &sx, &sy);
            int b, g, r;
            sample_rgb565_bilinear(frame, w, h, sx, sy, &b, &g, &r);
            if (roi_is_plate_color(r, g, b)) { uu[n++] = u; break; }
        }
    }
    if (n == 0) return -1.0f;
    if (n == 1) return uu[0];
    if (n == 2) return 0.5f * (uu[0] + uu[1]);
    if (uu[1] < uu[0]) { const float t = uu[0]; uu[0] = uu[1]; uu[1] = t; }
    if (uu[2] < uu[0]) { const float t = uu[0]; uu[0] = uu[2]; uu[2] = t; }
    if (uu[2] < uu[1]) { const float t = uu[1]; uu[1] = uu[2]; uu[2] = t; }
    return uu[1];                                       // 三个数取中位数
}
/**
 * P5.66: 从车牌四边形里抠出「省字那一格」-> 32x64 的省字模型输入 (RGB 序, 用省字 LUT 量化)。
 *   u 方向取 [0, 1/7.35] —— 省字在整牌长边方向占的那一段 (与训练/PC 复算逐参数一致);
 *   v 方向取满 [0,1] —— 省字高度就是车牌高度;
 *   每个输出像素按它在源图上的足迹做 nx*ny 次双线性取样再平均 (同 sample_quad_to_input, 不这样会欠采样走样);
 *   **不经过 crop_photometric_norm** —— 那个饱和度 x2 是给 94x24 主模型调出来的, 省字模型的
 *   训练素材是原色, 加进去就是域不匹配.
 */
static void sample_prov_to_input(const uint8_t *frame, int w, int h,
                                 const float *qx, const float *qy, float u_base,
                                 int8_t *dst, const int8_t *lut) {
    const float du = PROV_U_SPAN / (float)PROV_W;
    const float dv = 1.0f / (float)PROV_H;
    for (int y = 0; y < PROV_H; y++) {
        const float v0 = (float)y * dv;
        for (int x = 0; x < PROV_W; x++) {
            // P5.67: u_base = 这扇窗口的左边界 (由蓝面锚点/候选决定), 老做法就是 0
            const float u0 = u_base + (float)x * du;
            float ax, ay, bx, by, cx, cy;
            quad_eval(qx, qy, u0, v0, &ax, &ay);
            quad_eval(qx, qy, u0 + du, v0, &bx, &by);
            quad_eval(qx, qy, u0, v0 + dv, &cx, &cy);
            const float ex = bx - ax, ey = by - ay;
            const float fx = cx - ax, fy = cy - ay;
            int nx = (int)(sqrtf(ex * ex + ey * ey) + 0.5f);
            int ny = (int)(sqrtf(fx * fx + fy * fy) + 0.5f);
            if (nx < 1) nx = 1; else if (nx > SS_MAX) nx = SS_MAX;
            if (ny < 1) ny = 1; else if (ny > SS_MAX) ny = SS_MAX;
            float acc[3] = {0, 0, 0};
            for (int j = 0; j < ny; j++) {
                const float vv = v0 + (j + 0.5f) * dv / (float)ny;
                for (int i = 0; i < nx; i++) {
                    const float uu = u0 + (i + 0.5f) * du / (float)nx;
                    float sx, sy;
                    quad_eval(qx, qy, uu, vv, &sx, &sy);
                    int b, g, r;
                    sample_rgb565_bilinear(frame, w, h, sx, sy, &b, &g, &r);
                    acc[0] += (float)b; acc[1] += (float)g; acc[2] += (float)r;
                }
            }
            const float inv = 1.0f / (float)(nx * ny);
            int bb = (int)(acc[0] * inv + 0.5f);
            int gg = (int)(acc[1] * inv + 0.5f);
            int rr = (int)(acc[2] * inv + 0.5f);
            if (bb > 255) bb = 255; else if (bb < 0) bb = 0;
            if (gg > 255) gg = 255; else if (gg < 0) gg = 0;
            if (rr > 255) rr = 255; else if (rr < 0) rr = 0;
            // P5.67: 顺手把这块 patch 的原色抄一份 —— 详细模式(或复核没过门槛时)会把它发回串口
            if (g_prov_rgb != nullptr) {
                uint8_t *pd = g_prov_rgb + ((size_t)y * PROV_W + x) * 3;
                pd[0] = (uint8_t)rr;
                pd[1] = (uint8_t)gg;
                pd[2] = (uint8_t)bb;
            }
            // 通道序 R,G,B —— 与训练时的 im[:,:,::-1] (BGR->RGB) 一致; 三个通道的 LUT 相同
            int8_t *o = dst + ((size_t)y * PROV_W + x) * 3;
            o[0] = lut[rr];
            o[1] = lut[gg];
            o[2] = lut[bb];
        }
    }
}

// P5.66: 省字复核模型句柄 (app_main 里建; g_prov_model == nullptr 表示没启用, 整条支路跳过)
static dl::Model *g_prov_model = nullptr;
static dl::TensorBase *g_prov_input = nullptr;
static dl::TensorBase *g_prov_out = nullptr;   // float 反量化输出 [1,31]
static int8_t *g_prov_lut = nullptr;

/**
 * P5.6: 沿"选中的四边形"自己的高/宽方向, 量一遍车牌色覆盖率 —— 用来回答"框到底套得准不准"。
 *   顶/底两端明显偏低 => 框上下多吃了一条(4px 格点外扩 / 背景被并进来), 该按这个比例收紧;
 *   从头到尾都有 ~90%  => 框是紧的, 那"比例偏低"就是斜着拍(车牌在画面里是梯形)造成的,
 *                        该改真四角透视校正, 而不是收紧(收紧会切掉省份字)。
 * 纯诊断, 不参与任何判断; 只在详细模式 + 结果可疑时打 (每次 12 行)。
 */
static void roi_probe_profile(const uint8_t *fbuf, int w, int h, const float *qx, const float *qy,
                              float v_lo, float v_hi) {
    const int UN = 9, VN = 12;
    ESP_LOGI(TAG, "沿车牌高度方向的蓝色覆盖率 (12 段 x 9 点; 两端低=框太胖, 全程高=斜拍; 量的是收边后的区间):");
    for (int j = 0; j < VN; j++) {
        const float v = v_lo + ((float)j + 0.5f) / (float)VN * (v_hi - v_lo);
        int hit = 0;
        char pat[UN + 1];
        for (int i = 0; i < UN; i++) {
            const float u = ((float)i + 1.0f) / ((float)UN + 1.0f);
            float sx, sy;
            quad_eval(qx, qy, u, v, &sx, &sy);
            const int x = (int)(sx + 0.5f), y = (int)(sy + 0.5f);
            char sym = '-';
            if (x >= 0 && y >= 0 && x < w && y < h) {
                int b, g, r;
                sample_rgb565_bilinear(fbuf, w, h, sx, sy, &b, &g, &r);
                if (roi_is_plate_color(r, g, b)) {
                    sym = '#';   // 车牌色
                    hit++;
                } else {
                    // P5.8: 把"不是蓝"再分两类 —— 这决定了该怎么收框:
                    //   W = 亮但不饱和(白字/白边框) => 这是车牌自己的东西, 千万别切掉;
                    //   - = 其他(暗背景/车身/杂物)   => 这才是框多吃的部分, 该收掉。
                    const int v3 = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
                    const int m3 = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
                    const int sat = (v3 > 0) ? (255 * (v3 - m3)) / v3 : 0;
                    sym = (v3 >= 120 && sat < 60) ? 'W' : '-';
                }
            }
            pat[i] = sym;
        }
        pat[UN] = '\0';
        ESP_LOGI(TAG, "  v=%3.0f%%  %d/%d  %s%s", v * 100.0f, hit, UN, pat,
                 (j == 0 || j == VN - 1) ? "   <== 边界" : "");
    }
}

/**
 * P5.9: 沿"短边方向"(v) 实测蓝色覆盖, 找出车牌真正的上下边界 => 把框收紧。
 * 依据(现场日志 §23): 12 段剖面里最上段只有 1/9 是车牌色、最下段 3/9, 而中间各段 5~8/9;
 *   同时框的长短边比中位只有 2.61 (真车牌 ~3.14) —— 说明框在短边方向比车牌高出一截, 字符被压扁。
 * 做法: 从两端往里走, 找到"从这里开始连续 3 行覆盖率都 >= 4/9"的位置当边界(允许 1 行抖动);
 *   单端最多收 ROI_VTRIM_MAX, 收完至少留 ROI_VTRIM_MIN_KEEP —— 任何一条不满足就这一端/整块不收,
 *   宁可不收也不能把车牌切掉 (白边框被收掉无害, 切到字符就是灾难)。
 * 纯几何, 与"框里是什么"无关, 因此屏幕素材上量的结论对实体牌同样成立。
 */
static void roi_trim_short_axis(const uint8_t *fbuf, int w, int h, const float *qx, const float *qy,
                                float *v_lo, float *v_hi) {
    const int N = ROI_VTRIM_N, UN = ROI_VTRIM_UN;
    int cov[N];
    for (int j = 0; j < N; j++) {
        const float v = ((float)j + 0.5f) / (float)N;
        int hit = 0;
        for (int i = 0; i < UN; i++) {
            const float u = ((float)i + 1.0f) / ((float)UN + 1.0f);
            float sx, sy;
            quad_eval(qx, qy, u, v, &sx, &sy);
            const int x = (int)(sx + 0.5f), y = (int)(sy + 0.5f);
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            int b, g, r;
            sample_rgb565_bilinear(fbuf, w, h, sx, sy, &b, &g, &r);
            if (roi_is_plate_color(r, g, b)) hit++;
        }
        cov[j] = hit;
    }
    *v_lo = 0.0f;
    *v_hi = 1.0f;
    // 判"这一行属于车牌"的门槛: 9 点里 >= 4 点是车牌色。
    // 不用"半数"是因为车牌中间那些穿过白字的行本来就会掉到 4~5 点(白字不是车牌色);
    // 而框两端多吃进来的背景行基本是 0~1 点, 两者差得很开, 4 足够区分。
    const int need = 4;
    int jt = -1, jb = -1;
    for (int j = 0; j + 2 < N; j++) {
        if (cov[j] >= need && cov[j + 1] >= need && cov[j + 2] >= need) { jt = j; break; }
    }
    for (int j = N - 1; j - 2 >= 0; j--) {
        if (cov[j] >= need && cov[j - 1] >= need && cov[j - 2] >= need) { jb = j; break; }
    }
    if (jt < 0 || jb < 0) return;                 // 找不到可信边界 => 不收
    float lo = (float)jt / (float)N;              // 第 jt 行的上边界就是 jt/N
    float hi = (float)(jb + 1) / (float)N;        // 第 jb 行的下边界就是 (jb+1)/N
    if (lo < 0.0f) lo = 0.0f;
    if (hi > 1.0f) hi = 1.0f;
    if (lo > ROI_VTRIM_MAX) lo = 0.0f;            // 上端收得太多 => 只这一端不收
    if (1.0f - hi > ROI_VTRIM_MAX) hi = 1.0f;     // 下端同理
    if (hi - lo < ROI_VTRIM_MIN_KEEP) return;     // 两端合起来剩太少 => 整块不收
    *v_lo = lo;
    *v_hi = hi;
}

// P5.61: 这里原来是 roi_probe_utrim() —— 沿长边(u)方向量"蓝色带"并把取样框左端收窄。
//   **整个函数已删除**: 复算证明它把省字切掉(收左端 44% vs 不收 75%, 且 0 帧有帮助)。
//   现在取样框就是定位框原样 + g_pad_u_l/g_pad_u_r 留白。详见文档第四十五节。

/**
 * 一次完整识别: 取帧 -> 预处理 -> 推理 -> 解码 -> 串口打印
 * 返回 true 表示这一次真的跑完了推理
 * verbose=true  : 详细模式 (开机 / 按 BOOT), 打掩码 + 1:1 缩略图 + 输入统计 (重, 串口慢)
 * verbose=false : 精简模式 (定时连续), 只打 ROI 行 / 摆正行 / RESULT, 保证实时不被串口拖住
 */
static bool recognize_once(dl::Model *model,
                           dl::TensorBase *model_input,
                           dl::TensorBase *output_float,
                           int8_t *norm_lut,
                           const char *trigger,
                           bool verbose,
                           bool must_report) {
    const int fn = ++g_frame_no;   // P5.12: 帧号 —— 之后每行日志都带 #n, 便于对照
    // P5.77: must_report = 这一帧是"被点名拍的"(AT+RUN) —— 没出结果时收尾必打一条人话原因。
    bool        reported_ok = false;
    const char *fail_reason = NULL;
    char        fail_buf[192];
    g_crop_q.valid = 0;            // P5.24: 本帧的裁剪块质量由预处理那一趟填; 先清掉, 免得日志报上一帧的数字
    if (verbose) ESP_LOGI(TAG, "------------ 会话: %s (帧 #%d) ------------", trigger, fn);

    camera_discard_frames(CAM_DISCARD_FRAMES, CAM_DISCARD_DELAY_MS);

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "拍照失败");
        if (must_report) print_fail_block("取帧失败 —— 摄像头没回帧 (线松/供电不足/驱动异常)", fn);
        return false;
    }

    bool ok = false;
    roi_box_t box = {};        // P5.12: 提到 do 外面 —— 帧尾发预览图时还要用
    bool has_roi = false;
    do {
        if (fb->format != PIXFORMAT_RGB565) {
            ESP_LOGE(TAG, "帧格式不是 RGB565: %d", (int)fb->format);
            fail_reason = "帧格式不是 RGB565 (摄像头配置被改坏了)";
            break;
        }
        // esp-dl 假设数据是紧凑的 (无行填充), 不成立就必须报错而不是将错就错
        const size_t expect = (size_t)fb->width * fb->height * 2;
        if (fb->len != expect) {
            ESP_LOGE(TAG, "帧长度异常: len=%u 期望=%u (存在行填充?)",
                     (unsigned)fb->len, (unsigned)expect);
            fail_reason = "帧长度异常 (存在行填充)";
            break;
        }

        // ---- ROI: 先在整帧里定位车牌; 定不到就不猜 ----
        // 模型训练集是 94x24 的"紧裁剪车牌"(见 data/official_train): 训练时把车牌四角摆正图
        // 直接 resize 成 94x24 (extract_ccpd.py:76-90 + load_data.py:42),
        // 把 640x480 整帧直接塞进去 => 车牌只占几个像素, 结果必然瞎猜。
        const int64_t t_roi = esp_timer_get_time();
        has_roi = roi_locate(fb->buf, (int)fb->width, (int)fb->height, &box, verbose);
        const int64_t roi_ms = (esp_timer_get_time() - t_roi) / 1000;
        if (!has_roi) {
            // P5.12: 一行说清"为什么跳过"; 掩码/整帧均值/候选表那些细节按 BOOT 才打
            ESP_LOGW(TAG, "#%d 跳过推理: %s (定位 %lld ms)", fn, g_roi_note, (long long)roi_ms);
            fail_reason = g_roi_note;
            break;
        }
        // 宽高比对照: 训练裁剪就是"车牌四角摆正图", 真实车牌本体约 3.1~3.4
        // (auto_crop_predict.py:75 的注释也写 3~3.4)。94x24=3.92 只是模型输入张量的比例,
        // 不是要拿框去凑的目标。框比 3.1 矮很多 => 竖直方向多含了背景 => 字符被压扁。
        if (verbose) {
            ESP_LOGI(TAG, "ROI: x[%d,%d) y[%d,%d) %dx%d 宽高比 %.2f (真车牌约 3.1~3.4, 模型输入 94x24) 有效填充 %.0f%% 占屏 %.1f%% | 定位 %lld ms",
                     box.x1, box.x2, box.y1, box.y2, box.x2 - box.x1, box.y2 - box.y1,
                     box.ratio, box.density * 100.0f, box.area_pct, (long long)roi_ms);
        }

        dl::image::img_t src = {fb->buf, (uint16_t)fb->width, (uint16_t)fb->height,
                                dl::image::DL_IMAGE_PIX_TYPE_RGB565};
        dl::image::img_t dst = {model_input->data,
                                (uint16_t)model_input->shape[2],   // W = 94
                                (uint16_t)model_input->shape[1],   // H = 24
                                dl::image::DL_IMAGE_PIX_TYPE_RGB888_QINT8};
        const uint32_t caps = DL_IMAGE_CAP_RGB565_BIG_ENDIAN | DL_IMAGE_CAP_RGB_SWAP;
        // crop_area 语义 = {x1, y1, x2, y2}: 一次完成 裁剪 + 双线性缩放 + 通道序 + 归一化量化
        // P5.13/P5.32: 与旋转路径一致, 轴对齐回退也要外扩留白(并夹回画面内); 长边两端同样不对称。
        const int padl = (int)((float)(box.x2 - box.x1) * g_pad_u_l);
        const int padr = (int)((float)(box.x2 - box.x1) * g_pad_u_r);
        const int pady = (int)((float)(box.y2 - box.y1) * ROI_PAD_V);
        int ax1 = box.x1 - padl, ay1 = box.y1 - pady;
        int ax2 = box.x2 + padr, ay2 = box.y2 + pady;
        if (ax1 < 0) ax1 = 0;
        if (ay1 < 0) ay1 = 0;
        if (ax2 > (int)fb->width) ax2 = (int)fb->width;
        if (ay2 > (int)fb->height) ay2 = (int)fb->height;
        const std::vector<int> crop_area = {ax1, ay1, ax2, ay2};

        float scale_x = 0, scale_y = 0;
        int ss_nx = 0, ss_ny = 0;   // P5.5: 摆正路径实际用的子采样数 (0 = 走了 esp-dl 缩放)
        int64_t t_pre = esp_timer_get_time();
        float v_lo = 0.0f, v_hi = 1.0f;   // P5.9: 短边方向的取样区间 (默认不收)
        float norm_s = 1.0f;              // P5.10: 裁剪块光度归一化的倍数 (1.0 = 没动)
        if (box.rot_ok) {
            // P5.9: 先实测"蓝色区域的上下边界", 再按这个区间采样 —— 修的是"框短边方向偏胖, 字符被压扁"
            roi_trim_short_axis(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy, &v_lo, &v_hi);
            // 有倾斜: 用 PCA 拟合出的旋转矩形把车牌摆正 (等价训练链路的 warpPerspective)
            if (verbose) {
                ESP_LOGI(TAG, "摆正(旋转矩形): 长宽比 %.2f 收边后 %.2f (真车牌约 3.1~3.4) 填充 %.0f%% 倾角 %.1f° 收边 v=[%.0f%%,%.0f%%] 四角 (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)",
                         box.rot_ratio, box.rot_ratio / (v_hi - v_lo), box.rot_fill * 100.0f, box.rot_deg,
                         v_lo * 100.0f, v_hi * 100.0f,
                         box.qx[0], box.qy[0], box.qx[1], box.qy[1],
                         box.qx[2], box.qy[2], box.qx[3], box.qy[3]);
            }
            sample_quad_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                 0.0f, 1.0f, v_lo, v_hi,
                                 (int8_t *)model_input->data, norm_lut, &ss_nx, &ss_ny, &norm_s);
        } else {
            if (verbose) ESP_LOGW(TAG, "旋转拟合不可用 -> 退回轴对齐裁剪 (框宽高比 %.2f)", box.ratio);
            dl::image::resize(src, dst, dl::image::DL_IMAGE_INTERPOLATE_BILINEAR, caps, norm_lut,
                              crop_area, &scale_x, &scale_y);
        }
        int64_t pre_ms = (esp_timer_get_time() - t_pre) / 1000;

        // P5.16/P5.74: 生效档 3 就把"模型真正吃到的那块图"存下来。必须在这里做 —— run() 之后
        // 这块显存会被复用 (见下面那段警告), 那时候读到的已经是别的张量了。
        if (g_img_mode == 3) {
            const int8_t *cp = (const int8_t *)model_input->data;
            const size_t np = (size_t)IMG_W * IMG_H;
            for (size_t i = 0; i < np; i++) {
                const uint8_t b = g_inv_lut[(int)cp[i * 3 + 0] + 128];
                const uint8_t g = g_inv_lut[(int)cp[i * 3 + 1] + 128];
                const uint8_t r = g_inv_lut[(int)cp[i * 3 + 2] + 128];
                g_crop_u8[i * 3 + 0] = r;   // JPEG 源要 RGB 序 (和预览那条路一致)
                g_crop_u8[i * 3 + 1] = g;
                g_crop_u8[i * 3 + 2] = b;
            }
        }

        // S5: 车牌缩略图(94×24 二值 282B)每帧都产, 不看图片档位 —— 节点 AT+THUMB 取的就是它。
        // 同样必须在 run() 之前 (模型输入 run 后被显存复用)。
        thumb_build((const int8_t *)model_input->data);

        // P5.8: 裁剪块的三通道均值 —— 必须在这里取(run() 之后这块显存会被复用, 见下面那段警告)。
        // 用途: 把"拍得发白"和"认错字"对上号 (现场 B=229 G=179 R=103, 正常蓝牌底约 B≈200 G≈70 R≈40)。
        long cm_sb = 0, cm_sg = 0, cm_sr = 0;
        {
            const int8_t *cp = (const int8_t *)model_input->data;
            const size_t cn = (size_t)IMG_W * IMG_H;
            for (size_t i = 0; i < cn; i++) {
                cm_sb += cp[i * 3 + 0];
                cm_sg += cp[i * 3 + 1];
                cm_sr += cp[i * 3 + 2];
            }
        }
        const float cm_inv = 1.0f / (float)((size_t)IMG_W * IMG_H);
        const int cm_b = (int)((float)cm_sb * cm_inv + 127.0f);
        const int cm_g = (int)((float)cm_sg * cm_inv + 127.0f);
        const int cm_r = (int)((float)cm_sr * cm_inv + 127.0f);

        // ⚠⚠ 这几行必须在 run() 之前打印 (踩过的坑, 别再挪回去)
        // esp-dl 的内存管理器算完"图输入的最后一个消费者"就把这块显存还给了池子
        //   dl/model/src/dl_memory_manager_greedy.cpp:130  update_time(i+1) // free this tensor next step
        // 之后 run() 里的中间张量会复用这块内存 => run() 之后再读 model_input->data, 读到的是
        // 别的张量的数据。曾经因此把"裁剪块均值 B≈G≈R / 缩略图一片灰"当成证据, 误判成
        // "曝光/白平衡坏了" —— 那三行诊断当时全是假的。
        if (verbose) {   // 连续模式下这些太占串口 (1:1 缩略图 24 行 ≈ 200ms), 只在详细模式打
            ESP_LOGI(TAG, "帧 %ux%u %uB | 裁出 %dx%d (缩放 %.3f/%.3f)", (unsigned)fb->width,
                     (unsigned)fb->height, (unsigned)fb->len, box.x2 - box.x1, box.y2 - box.y1,
                     scale_x, scale_y);
            log_input_stats(model_input);
            if (box.rot_ok) {   // P5.6: 把"框套得准不准"量出来
                roi_probe_dominant_blue(fb->buf, (int)fb->width, (int)fb->height, &box);   // P5.29: 只打日志

                roi_probe_profile(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy, v_lo, v_hi);
            }
        }


        int64_t t_inf = esp_timer_get_time();
        model->run();
        int64_t inf_ms = (esp_timer_get_time() - t_inf) / 1000;

        output_float->assign(model->get_outputs().begin()->second);
        decode_report_t rep;   // P5.24: 顺带算出每一步的置信度 (不改变解码结果)
        std::string plate = greedy_decode_impl((const float *)output_float->data, &rep);
        // P5.13: 蓝牌永远是 7 位(省 + 字母 + 5 位)。外扩之后偶尔会从画面外沿的暗边上
        //   再"读"出一个字母挂在尾巴上 (实测 京Q06666 -> 京Q06666L / 京Q06666U, 21 帧里 12 帧)。
        //   蓝底时(裁剪块 B > G)把多出来的那个尾字符削掉; 绿牌是 8 位, 不动。
        //
        // P5.38: 原来这里判的是"字节数 == 10" —— 会误伤: 两个汉字 + 4 个 ASCII 也是 10 字节。
        //   现场 (2026-09-30 日志) 就撞上了: 豫FS8鄂8 -> 削成 豫FS8鄂, FSS鄂S -> 削成 FSS鄂,
        //   43 帧里 5 帧被这么砍成 5 个字, 越修越离谱。现在改成**按字符数**判:
        //   只有"1 个汉字 + 7 个 ASCII"(共 8 个字) 才认定是尾巴上多读了一个 ASCII, 削掉它。
        {
            int n_cjk = 0, n_ascii = 0;
            for (size_t i = 0; i < plate.size();) {
                const unsigned char c = (unsigned char)plate[i];
                if (c < 0x80) { n_ascii++; i += 1; }
                else if ((c & 0xE0) == 0xC0) { n_cjk++; i += 2; }
                else if ((c & 0xF0) == 0xE0) { n_cjk++; i += 3; }
                else { n_cjk++; i += 4; }
            }
            if (n_cjk == 1 && n_ascii == 7 && cm_b > cm_g) {
                if (verbose) ESP_LOGW(TAG, "语法修正: %s -> 去掉尾巴上多出的一位 (蓝牌应为 7 位)", plate.c_str());
                plate.pop_back();          // 多出来的那一字节必在末尾, 削 1 字节 = 削 1 个 ASCII 字符
            }
        }

        // P5.5: 把"这块牌实际被多少个源像素平均出来"打出来 —— 采样数 <1 说明牌太小(信息本就不够),
        // 而不是模型不行; 这个数字和 RESULT 一起看, 才能分清"欠采样"和"认字不行"。
        // P5.12: 连续模式把一帧压成"一行指标 + 一行结果"; 其余诊断只在按 BOOT 时打
        // P5.46: [兜底] 标记 —— 这一帧是靠"面积够大就收"的兜底档挑到框的 (正常帧不会出现)
        char rlx[48] = "";
        if (!box.rot_ok) strcat(rlx, " 轴对齐");
        if (g_roi_fallback) strcat(rlx, " [兜底]");
        if (ss_nx > 0) {
            // P5.61: 这里原来还打 [蓝带[左%~右%] 多吃[左%,右%]] —— 它是"长边收边"的量尺,
            //   收边删掉之后这个字段没有任何消费者, 一并去掉 (顺便省掉每帧 24x9 次采样)。
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 上下[%.0f%%,%.0f%%] | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",
                     fn, box.x2 - box.x1, box.y2 - box.y1, box.area_pct,
                     rlx,
                     v_lo * 100.0f, v_hi * 100.0f,
                     (long long)roi_ms, (long long)pre_ms, (long long)inf_ms,
                     ss_nx, ss_ny, norm_s, cm_b, cm_g, cm_r);

        } else {
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s | 定位%lld 预处理%lld 推理%lld ms",
                     fn, box.x2 - box.x1, box.y2 - box.y1, box.area_pct,
                     rlx,
                     (long long)roi_ms, (long long)pre_ms, (long long)inf_ms);
        }
        // P5.24: 结果后面直接跟置信度 —— 连续模式(每帧 2 行)也能一眼看出"这次是模型有把握还是瞎猜"
        // P5.46: **结果闸门** —— 模型说了什么不再直接当车牌上报。
        //   为什么必须加: 2026-10-01 那份 78 帧日志里 21 个错结果**全部**格式不合法
        //   (皖1 / 浙JTT / 粤11191 / 粤J191粤T ...), 唯一合法的 #82 就是对的。
        //   所以"格式合法"这条比几何门槛更能挡误检, 而且它是**事后**判的, 不会拦掉真车牌。
        // ---- P5.66: 省字复核 (第二个模型, 只看第一个字) ----
        //   只在"主模型确实读出了东西 + 四边形可用"时才跑; 连车牌都没读出来就不必花这 30ms。
        std::string plate_main = plate;   // 主模型原读 (日志里要对照)
        char main_head[8] = "?";
        if (!plate_main.empty()) {
            const unsigned char c0 = (unsigned char)plate_main[0];
            size_t hl = 1;
            if ((c0 & 0xF0) == 0xE0) hl = 3;
            else if ((c0 & 0xE0) == 0xC0) hl = 2;
            else if ((c0 & 0xF8) == 0xF0) hl = 4;
            if (hl > plate_main.size()) hl = plate_main.size();
            memcpy(main_head, plate_main.data(), hl);
            main_head[hl] = '\0';
        }
        int prov_id = -1, prov_id2 = -1, prov_ms = 0;
        float prov_p = 0.0f, prov_p2 = 0.0f;
        float prov_u = 0.0f;            // 最终采用的窗口左边界 (u)
        float prov_u_left = -1.0f;      // "蓝面左边缘"锚点扫描结果 (-1 = 没扫到)
        float prov_p_legacy = -1.0f;    // 老做法(窗口从框左角 u=0 开始)的置信, 只做对照
        int   prov_ncand = 0;
        float prov_cand_u[PROV_CAND_MAX + 2];
        float prov_cand_p[PROV_CAND_MAX + 2];
        if (g_prov_model != nullptr && !plate.empty() && box.rot_ok) {
            // P5.67: 候选窗口 —— [0] 是老做法 (u=0), 其余以"蓝面左边缘"为锚点向两侧展开
            prov_cand_u[prov_ncand++] = 0.0f;
            prov_u_left = prov_find_u_left(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy);
            if (prov_u_left >= 0.0f) {
                for (int j = -PROV_CAND_K; j <= PROV_CAND_K && prov_ncand < PROV_CAND_MAX + 2; j++) {
                    const float u = prov_u_left + (float)j * PROV_CAND_STEP;
                    if (u < -0.05f || u + PROV_U_SPAN > 1.05f) continue;   // 窗口整块跑到框外面去了
                    bool dup = false;
                    for (int m = 0; m < prov_ncand; m++) {
                        if (fabsf(u - prov_cand_u[m]) < 0.0030f) { dup = true; break; }
                    }
                    if (!dup) prov_cand_u[prov_ncand++] = u;
                }
            }
            float best_p = -1.0f;
            for (int c = 0; c < prov_ncand; c++) {
                sample_prov_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                     prov_cand_u[c], (int8_t *)g_prov_input->data, g_prov_lut);
                const int64_t t_prov = esp_timer_get_time();
                g_prov_model->run();
                prov_ms += (int)((esp_timer_get_time() - t_prov) / 1000);
                g_prov_out->assign(g_prov_model->get_outputs().begin()->second);
                const float *pl = (const float *)g_prov_out->data;
                int id = 0;
                for (int c2 = 1; c2 < PROV_CLASS; c2++) if (pl[c2] > pl[id]) id = c2;
                const float vmax = pl[id];
                float sum = 0.0f;
                for (int c2 = 0; c2 < PROV_CLASS; c2++) sum += expf(pl[c2] - vmax);
                const float p = (sum > 0.0f) ? (1.0f / sum) : 0.0f;      // top1 的 softmax
                prov_cand_p[c] = p;
                if (c == 0) prov_p_legacy = p;
                if (p > best_p) {
                    best_p = p;
                    prov_id = id;
                    prov_id2 = (id == 0) ? 1 : 0;
                    for (int c2 = 0; c2 < PROV_CLASS; c2++)
                        if (c2 != id && pl[c2] > pl[prov_id2]) prov_id2 = c2;
                    prov_p = p;
                    prov_p2 = (sum > 0.0f) ? expf(pl[prov_id2] - vmax) / sum : 0.0f;
                    prov_u = prov_cand_u[c];
                }
            }
            // 把"赢了的那扇窗"重新抠一遍进输入张量 —— 这样发回串口的 patch 就是模型真正吃到的那块
            sample_prov_to_input(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy,
                                 prov_u, (int8_t *)g_prov_input->data, g_prov_lut);
        }
        // 融合: 省字以复核模型为准; 把握不足就**不动**主模型的读法 (宁可不说, 别改错)
        bool prov_used = false;
        if (prov_id >= 0 && prov_p >= PROV_CONF_MIN) {
            const unsigned char c0 = (unsigned char)plate[0];
            size_t hl = 1;
            if ((c0 & 0xF0) == 0xE0) hl = 3;
            else if ((c0 & 0xE0) == 0xC0) hl = 2;
            else if ((c0 & 0xF8) == 0xF0) hl = 4;
            if (hl > plate.size()) hl = plate.size();
            plate = std::string(PROV_CHARS[prov_id]) + plate.substr(hl);
            prov_used = true;
        }
        char provbuf[224] = "";
        if (prov_id >= 0) {
            // 窗口对照字段: 采用了哪扇窗 / 锚点在哪 / 老做法多自信 / 一共试了几扇 —— 一眼看出这帧是不是靠锚点救回来的
            char winbuf[80] = "";
            if (prov_u_left >= 0.0f) {
                snprintf(winbuf, sizeof(winbuf), " | 窗u%+.3f/锚%+.3f/旧窗%.0f%%/试%d窗",
                         prov_u, prov_u_left, prov_p_legacy * 100.0f, prov_ncand);
            }
            snprintf(provbuf, sizeof(provbuf), " | 省字复核 %s %.0f%% (次选 %s %.0f%%, %dms)%s 主模型首字 %s%s",
                     PROV_CHARS[prov_id], prov_p * 100.0f, PROV_CHARS[prov_id2], prov_p2 * 100.0f, prov_ms,
                     prov_used ? " 已采用 *" : " 未过门槛, 保留 *", main_head, winbuf);
        }
        if (verbose && prov_id >= 0) {
            char candbuf[200] = "";
            for (int c = 0; c < prov_ncand; c++) {
                char one[28];
                snprintf(one, sizeof(one), "%su%+.3f:%.0f%%", (c == 0) ? "" : " ",
                         prov_cand_u[c], prov_cand_p[c] * 100.0f);
                strncat(candbuf, one, sizeof(candbuf) - strlen(candbuf) - 1);
            }
            ESP_LOGI(TAG, "省字复核: 从原图 ROI %dx%d 的四边形里抠省字格 (u 窗宽 %d%%) -> %dx%d | 实读 %s %.0f%% (次选 %s %.0f%%) | 主模型首字 %s | %s | 试点 %d 个, 共 %d ms",
                     box.x2 - box.x1, box.y2 - box.y1, (int)(PROV_U_SPAN * 100.0f + 0.5f), PROV_W, PROV_H,
                     PROV_CHARS[prov_id], prov_p * 100.0f,
                     PROV_CHARS[prov_id2], prov_p2 * 100.0f, main_head,
                     prov_used ? "已采用复核结果" : "未过 70% 门槛, 保留主模型", prov_ncand, prov_ms);
            ESP_LOGI(TAG, "省字窗口扫描: 蓝面左边缘锚点 u_left=%+.3f (占总长 %+.1f%%, 相当于窗宽的 %+.0f%%) | 候选(窗宽 %.0f%% 时各自的实测置信): %s | 取 u%+.3f (老做法 u=0 时 %.0f%%)",
                     prov_u_left, prov_u_left * 100.0f, prov_u_left / PROV_U_SPAN * 100.0f,
                     PROV_U_SPAN * 100.0f, candbuf, prov_u, prov_p_legacy * 100.0f);
        }
        // P5.67: 把省字 patch 发回串口 —— 详细模式每帧都发; 连续模式只在"复核没过门槛"时发 (限流 10s)
        if (prov_id >= 0) {
            static int64_t last_prov_dump_us = 0;
            const int64_t now_dump_us = esp_timer_get_time();
            if (verbose || (prov_p < PROV_CONF_MIN && now_dump_us - last_prov_dump_us > PROV_DUMP_MIN_GAP_US)) {
                last_prov_dump_us = now_dump_us;
                img_tx_send_prov(PROV_DUMP_ZOOM);
            }
        }

        const bool fmt_ok = (!plate.empty()) && plate_looks_valid(plate);
        if (plate.empty()) {
            ESP_LOGW(TAG, "#%d >>> RESULT: (未识别到车牌) | 置信 %.0f%%%s",
                     fn, rep.mean_top1 * 100.0f, provbuf);
            fail_reason = "模型认为这块图里没有车牌 (18 步全空白) —— 框可能套歪, 或字形太糊";
        } else if (fmt_ok) {
            ESP_LOGI(TAG, "#%d >>> RESULT: %s | 置信 %.0f%% (最弱步 %.0f%%)%s", fn, plate.c_str(),
                     rep.mean_top1 * 100.0f, rep.min_top1 * 100.0f, provbuf);
            reported_ok = true;
        } else {
            ESP_LOGW(TAG, "#%d >>> 疑似误检, 不作为车牌上报: %s | 置信 %.0f%% (格式不合法: 应为 7~8 位, 省字开头)%s",
                     fn, plate.c_str(), rep.mean_top1 * 100.0f, provbuf);
            snprintf(fail_buf, sizeof(fail_buf),
                     "模型认出的字符 \"%s\" 不合法 (应为 7~8 位, 省字开头)", plate.c_str());
            fail_reason = fail_buf;
        }

        // P5.76: 认出来就补一个"结果块" —— 简单模式(默认)下这就是全部日志, 详细模式里它当收尾摘要。
        //   其余一律静默: 没认出的帧 / 跳过推理的帧 / 各种警告, 简单模式一个字都不打。
        if (fmt_ok) {
            print_plate_block(plate.c_str(), rep.mean_top1 * 100.0f, fn, box.x1, box.y1, box.x2, box.y2);
        }

        // P5.68: 结果出来就推给"节点链路" —— 口径和日志的 RESULT 一致: 只有语法合法的车牌才推;
        //   每帧都发一行 $PLATE; 跳过推理 / 疑似误检的帧由 recognize_frame() 兜底补一行(车牌字段 -),
        //   保证"跑了一帧就一定有一行", 节点不会干等超时。
        node_link_report_plate(plate.c_str(), fmt_ok, (int)(rep.mean_top1 * 100.0f + 0.5f), fn);

        // P5.24: 详细模式把"模型到底有多确定 + 这块图本身行不行"摊开 ——
        //   这是分清"图不行(该改定位/预处理)"和"模型不行(只能靠素材微调)"的关键证据。
        if (verbose) {
            const float conf = rep.mean_top1 * 100.0f;
            if (rep.min_top1_t >= 0) {
                ESP_LOGI(TAG, "置信度: 字符步平均 %.0f%% | 最低 %.0f%% (第 %d 步) | 领先<10%% 的模糊步 %d 个 | 折叠后 %d 个字符 (%d 个非空白步 / 18)",
                         conf, rep.min_top1 * 100.0f, rep.min_top1_t, rep.ambig, rep.nseg, rep.nchar_steps);
                if (rep.ambig >= 3) {
                    ESP_LOGW(TAG, "   模糊步偏多 (>=3 个) => 模型在这几个字上没底; 通常是裁剪块偏糊/偏曝, 或这种字形它没见过");
                }
            } else {
                ESP_LOGW(TAG, "置信度: 18 步全是空白 —— 模型认为这块图里没有车牌 (裁剪块/框有问题, 或车牌太糊太小)");
            }
            ESP_LOGI(TAG, "时间步(18 步, _=空白): %s", rep.seq.c_str());
            ESP_LOGI(TAG, "每步 top1(次选)%%: %s", rep.steps.c_str());
            if (g_crop_q.valid) {
                ESP_LOGI(TAG, "裁剪块质量: 亮度 p5=%d p95=%d 跨度=%d | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 饱和度 x%.2f",
                         g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.spread, g_crop_q.over_pct,
                         g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                         CROP_SAT_GAIN);
                if (g_crop_q.sharp < 5.0f) {
                    ESP_LOGW(TAG, "   锐度偏低 (<5) => 这块图本身是糊的, 认错很正常 (先解决对焦/手抖/距离, 再谈模型)");
                }
            } else {
                ESP_LOGW(TAG, "裁剪块质量: 无 —— 这一帧走的是轴对齐硬裁, 不经过光度归一化 (斜牌这样裁必糊)");
            }
            // 轴对齐回退时不经过子采样/光度归一化, 这两个数字无意义 —— 明确写"n/a", 免得看成 0 是故障
            char samp[32], sharpbuf[16];
            if (ss_nx > 0) snprintf(samp, sizeof(samp), "%dx%d", ss_nx, ss_ny);
            else           snprintf(samp, sizeof(samp), "n/a");
            if (g_crop_q.valid) snprintf(sharpbuf, sizeof(sharpbuf), "%.1f", g_crop_q.sharp);
            else                snprintf(sharpbuf, sizeof(sharpbuf), "n/a");
            ESP_LOGI(TAG, "本帧小结: 路径=%s | 框 %dx%d 比例 %.2f 有效填充 %.0f%% | 子采样 %s | 置信 %.0f%% (最弱 %.0f%%) | 锐度 %s | 结果 %s",
                     box.rot_ok ? "摆正" : "轴对齐硬裁",
                     box.x2 - box.x1, box.y2 - box.y1, box.ratio, box.density * 100.0f,
                     samp, conf, rep.min_top1 * 100.0f, sharpbuf,
                     plate.empty() ? "(未识别到)" : plate.c_str());
        }

        // 结果不像车牌时补打一次掩码: 这是事后唯一能确认「框到底有没有套住车牌」的证据。
        // P5.4: 这里除了掩码, 还要把"喂给模型的裁剪块"打出来 —— 缩略图读不出字, 就是裁剪/定位的锅;
        // 读得出字却认错, 才是模型的锅。这两者必须分开看, 否则只能瞎猜。
        // P5.12: 这段是之前最大的刷屏源(掩码 60 行 + 覆盖率 12 行 + 缩略图 24 行 ≈ 8 KB ≈ 0.7 s)。
        // 连续模式下不再自动打, 想看就按 BOOT 走详细模式。
        // P5.22: 倾斜 >=8° 的帧也把"模型看到的裁剪块"打出来 —— 诊断倾斜问题就靠它
        const bool tilted_frame = (fabsf(box.rot_deg) >= 8.0f);
        static int mask_result_budget = 12;
        if (verbose && mask_result_budget > 0 && (plate.empty() || !plate_looks_valid(plate) || tilted_frame)) {
            mask_result_budget--;
            ESP_LOGW(TAG, "结果不像车牌, 补打一次掩码 (本机还剩 %d 次)", mask_result_budget);
            // P5.26: 这里原来还重打一遍缩略图和覆盖率剖面 —— 前者是死代码, 后者在预处理那一趟
            //   已经打过完全相同的一份 (同样的框), 纯属重复。
            roi_dump_mask(roi_cell_snap, ROI_CELL_NEED, &box);
        }

        // P5.24: 连续模式下"结果可疑就自动补打诊断" —— 不用按 BOOT 也不会错过出问题的那一帧。
        //   只打文字(置信度/路径/质量), 不打掩码和缩略图那两块 ASCII 图 (它们才是刷屏元凶, 长按 BOOT 才有)。
        //   限流 4 s 一次, 免得一屋子可疑帧把串口灌满。
#if AUTO_DIAG_ON_SUSPECT
        if (!verbose) {
            const bool nothing = (rep.nchar_steps == 0);                     // 18 步全空白
            const bool low_conf = (rep.nchar_steps > 0) && (rep.mean_top1 < 0.70f);
            const bool bad_fmt = (!plate.empty()) && !plate_looks_valid(plate);
            if (nothing || low_conf || rep.ambig >= 3 || tilted_frame || bad_fmt) {
                static int64_t last_auto_diag_us = 0;
                const int64_t now_us = esp_timer_get_time();
                if (now_us - last_auto_diag_us > 4000000) {
                    last_auto_diag_us = now_us;
                    ESP_LOGW(TAG, "#%d !! 这帧可疑, 自动补打诊断 (%s%s%s%s%s) —— 同一原因 4s 内只报一次",
                             fn,
                             nothing ? "一个字符都没认出 " : "",
                             low_conf ? "置信偏低 " : "",
                             (rep.ambig >= 3) ? "模糊步偏多 " : "",
                             tilted_frame ? "牌照明显倾斜 " : "",
                             bad_fmt ? "结果格式不对 " : "");
                    ESP_LOGI(TAG, "#%d 置信: 字符步均 %.0f%% | 最弱 %.0f%% (第 %d 步) | 模糊步 %d 个 | 非空白步 %d/18",
                             fn, rep.mean_top1 * 100.0f, rep.min_top1 * 100.0f, rep.min_top1_t,
                             rep.ambig, rep.nchar_steps);
                    ESP_LOGI(TAG, "#%d 每步 top1(次选)%%: %s", fn, rep.steps.c_str());
                    if (box.rot_ok) {
                        ESP_LOGI(TAG, "#%d 路径: 摆正(四边形采样) | 倾角 %+.1f° 旋转比例 %.2f 旋转填充 %.0f%% | 四角 (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)",
                                 fn, box.rot_deg, box.rot_ratio, box.rot_fill * 100.0f,
                                 box.qx[0], box.qy[0], box.qx[1], box.qy[1],
                                 box.qx[2], box.qy[2], box.qx[3], box.qy[3]);
                    } else {
                        ESP_LOGI(TAG, "#%d 路径: 轴对齐硬裁 | 倾角 %+.1f° | 旋转拟合失败原因: %s | 外接框比例 %.2f 有效填充 %.0f%% (斜牌这样裁必糊)",
                                 fn, box.rot_deg, box.rot_why ? box.rot_why : "?", box.ratio,
                                 box.density * 100.0f);
                    }
                    if (g_crop_q.valid) {
                        ESP_LOGI(TAG, "#%d 质量: 亮度跨度 %d (p5=%d p95=%d) | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 饱和度 x%.2f",
                                 fn, g_crop_q.spread, g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.over_pct,
                                 g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                                 CROP_SAT_GAIN);
                    } else {
                        ESP_LOGI(TAG, "#%d 质量: n/a (轴对齐回退, 这一帧没走光度归一化)", fn);
                    }
                }
            }
        }
#endif
        ok = true;
    } while (0);

    // P5.77: 被点名拍的那一帧(AT+RUN)没出结果 -> 必打一条原因 (连续模式不打, 免得刷屏)
    if (must_report && !reported_ok) print_fail_block(fail_reason, fn);

    // P5.12: 不管有没有定位到车牌, 都把这一帧(缩一半)编码成 JPEG 发给串口助手预览。
    // 没定位到也发 —— 正好让你看见"相机到底看见了什么", 比看文字直观。
    // P5.14: 绿框只在详细模式(开机/按 BOOT)那一帧画 —— 那帧是专门用来看"框套得准不准"的;
    //        定时连续帧不画框, 因为那些才是要采下来当训练素材的图 (画上去还得再擦一遍, 擦不干净)。
    if (g_img_mode == 3) {
        // 模式 3 发的是"喂给模型的那张小图": 这一帧没跑到推理(没定位到车牌)就没有东西可发。
        // 说一句, 免得以为图片输出坏了 —— 限制 5 s 一次, 不刷屏。
        if (ok) {
            img_tx_send_crop();
        } else {
            static int64_t last_hint_us = 0;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_hint_us > 5000000) {
                last_hint_us = now_us;
                ESP_LOGW(TAG, "模式 3: 这一帧没定位到车牌, 没有 94x24 输入块可发 (只发有牌的那几帧)");
            }
        }
    } else if (g_img_mode == 1 || g_img_mode == 2) {
        img_tx_send(fb->buf, (int)fb->width, (int)fb->height,
                   (has_roi && g_img_mode == 2) ? &box : nullptr);
    }
    // 模式 0 = 关: 一个字都不发 —— 默认 / 接真节点 / 嫌刷屏时用这档

    esp_camera_fb_return(fb);
    return ok;
}

/** 无摄像头时的自检: 用内嵌样张 test_input.bin 复现 P4 的回归验证 */
static void run_embedded_selfcheck(dl::Model *model,
                                   dl::TensorBase *model_input,
                                   dl::TensorBase *output_float) {
    ESP_LOGW(TAG, "内嵌样张自检 (test_input.bin, 标签 沪AMS087)");

    dl::TensorBase *input_tensor = new dl::TensorBase(
        {1, IMG_H, IMG_W, 3}, (const void *)test_input_bin, 0, dl::DATA_TYPE_FLOAT);
    model_input->assign(input_tensor);   // 内部完成 float -> int8 量化

    int64_t t = esp_timer_get_time();
    model->run();
    ESP_LOGI(TAG, "inference: %lld ms", (long long)((esp_timer_get_time() - t) / 1000));

    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGI(TAG, ">>> RESULT: %s", plate.c_str());

    delete input_tensor;
}

/** 简版 FNV-1a: 只回答"这块内存有没有被改过" */
static uint32_t mem_fnv32(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void log_input_tensor(const char *when, dl::TensorBase *t) {
    ESP_LOGW(TAG, "输入张量[%s]: data=%p shape=%s dtype=%s exp=%d", when, t->data,
             dl::vector_to_string(t->shape).c_str(), t->get_dtype_string(), t->exponent);
}

/** P5.50 自检探针: 一张内嵌的 float32 裸张量走"assign 量化 -> run -> CTC 解码"。
 *  P5.51: 顺带校验输入缓冲在 run() 前后有没有被覆盖 (esp-dl 会把输入显存还给池子)。 */
static void run_probe_selfcheck(dl::Model *model,
                                dl::TensorBase *model_input,
                                dl::TensorBase *output_float,
                                const uint8_t *blob,
                                const char *label) {
    dl::TensorBase *input_tensor = new dl::TensorBase(
        {1, IMG_H, IMG_W, 3}, (const void *)blob, 0, dl::DATA_TYPE_FLOAT);
    model_input->assign(input_tensor);   // 内部完成 float -> int8 量化
    log_input_tensor(label, model_input);
    const size_t nb = (size_t)IMG_W * IMG_H * 3;
    const uint32_t ck0 = mem_fnv32(model_input->data, nb);
    int64_t t = esp_timer_get_time();
    model->run();
    const uint32_t ck1 = mem_fnv32(model_input->data, nb);
    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGW(TAG, ">>> 自检 [%s] -> %s   (%lld ms) | 输入校验 %08X -> %08X %s", label, plate.c_str(),
             (long long)((esp_timer_get_time() - t) / 1000), (unsigned)ck0, (unsigned)ck1,
             (ck0 == ck1) ? "(run 没动过输入)" : "*** run 之后输入被覆盖了 ***");
    delete input_tensor;
}

/** P5.51: 复现实时链路 —— 不走 assign(), 直接把 int8 写进 model_input->data
 *  (和 sample_quad_to_input 干的事一样)。若这条是坏的、上面那条是好的,
 *  说明问题出在"往哪块内存写 / 那块内存 run() 期间归谁"。 */
static void run_probe_direct_int8(dl::Model *model,
                                  dl::TensorBase *model_input,
                                  dl::TensorBase *output_float,
                                  const uint8_t *blob,
                                  const char *label) {
    const float *f = (const float *)blob;
    const size_t nb = (size_t)IMG_W * IMG_H * 3;
    int8_t *d = (int8_t *)model_input->data;
    for (size_t i = 0; i < nb; i++) {
        int v = (int)floorf(f[i] * 128.0f + 0.5f);
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        d[i] = (int8_t)v;
    }
    log_input_tensor(label, model_input);
    const uint32_t ck0 = mem_fnv32(d, nb);
    int64_t t = esp_timer_get_time();
    model->run();
    const uint32_t ck1 = mem_fnv32(d, nb);
    output_float->assign(model->get_outputs().begin()->second);
    std::string plate = greedy_decode((const float *)output_float->data);
    ESP_LOGW(TAG, ">>> 直写int8自检 [%s] -> %s   (%lld ms) | 输入校验 %08X -> %08X %s", label, plate.c_str(),
             (long long)((esp_timer_get_time() - t) / 1000), (unsigned)ck0, (unsigned)ck1,
             (ck0 == ck1) ? "(run 没动过输入)" : "*** run 之后输入被覆盖了 ***");
}

/**
 * P5.68: 包一层 recognize_once —— 保证「跑了一帧就一定给节点一行 $PLATE」。
 *   为什么: recognize_once 在「跳过推理」(画面里没定位到车牌)那条路是直接 return false 的,
 *   一行 $PLATE 都不会出; 节点那边就分不清「这帧没认出来」和「链路/摄像头死了」, 只能干等超时。
 *   这里拿上行计数对一下: 没出就补一行空结果(车牌字段 -, 置信度 0)。
 */
static bool recognize_frame(dl::Model *model,
                            dl::TensorBase *model_input,
                            dl::TensorBase *output_float,
                            int8_t *norm_lut,
                            const char *trigger,
                            bool verbose,
                            bool must_report) {
    const uint32_t before = node_link_uplink_count();
    const bool ok = recognize_once(model, model_input, output_float, norm_lut, trigger, verbose, must_report);
    if (node_link_uplink_count() == before) node_link_report_plate("", false, 0, g_frame_no);
    return ok;
}

/**
 * P5.68: 节点链路的外挂 AT 命令 —— 只有 main.cpp 知道的东西(详细模式/图片模式/取样几何档)放这里。
 * P5.74: 回复规范: 设置回 OK / 失败回 ERR:<原因>; 查询只回 +XXX:<值>; 命令后加 ? 看取值含义。
 *   以后再加调试命令只往这个函数加分枝, node_link.c 一个字都不用改。
 *   没处理的命令把 resp 留空 (node_link 会回 ERR)。
 */
static void nl_extra_cmd(const char *verb, const char *arg, char *resp, size_t resp_sz) {
    if (strcmp(verb, "LOG") == 0) {
        if (!arg) {
            snprintf(resp, resp_sz, "+LOG:%d", g_verbose_mode ? 1 : 0);
        } else if (*arg == '0' || *arg == '1') {
            g_verbose_mode = (*arg == '1');
            apply_log_levels();          // P5.76: 日志档位立刻跟着切
            snprintf(resp, resp_sz, "OK");
        } else {
            snprintf(resp, resp_sz, "ERR:用法 AT+LOG=0|1 (发 AT+LOG? 看每个值的含义)");
        }
    } else if (strcmp(verb, "IMG") == 0) {
        if (!arg) {
            // P5.74: 查询只回纯值, 不带任何解释 (要看每档含义发 AT+IMG?)
            snprintf(resp, resp_sz, "+IMG:%d", g_img_mode_want);
        } else {
            const int m = atoi(arg);
            if (*arg == '\0' || m < 0 || m > 3) {
                snprintf(resp, resp_sz, "ERR:用法 AT+IMG=0..3 (发 AT+IMG? 看每个值的含义)");
            } else {
                g_img_mode_want = m;
                img_mode_apply();        // P5.74: 立刻生效(跟控制权无关); 顺手把 img= 提醒同步给链路层
                snprintf(resp, resp_sz, "OK");
            }
        }
    } else if (strcmp(verb, "THUMB") == 0) {
        // S5: 节点取车牌缩略图 —— 把 $IMGD/$IMGB/$IMGE 行推给节点, 这行回复只是"受理"。
        // 注意命令名不叫 AT+IMG: AT+IMG=<0..3> 已被图片档位占用(节点口也发不了它, 见权限闸门)。
        if (!g_thumbValid) {
            snprintf(resp, resp_sz, "ERR:还没有图 (摄像头还没识别出过车牌)");
        } else {
            node_link_send_thumb(g_thumb, (int)sizeof(g_thumb), (int)g_thumbNo);
            snprintf(resp, resp_sz, "OK");
        }
    } else if (strcmp(verb, "RUN") == 0) {
        // P5.74: 这里回 OK 只是"受理了"—— node_link 会把它吞掉并记下是谁问的;
        //   真正拍照在主循环里做(模型句柄是 app_main 的局部量), 跑完直接发那帧的 $PLATE 行当回复
        g_run_request = true;
        snprintf(resp, resp_sz, "OK");
    } else if (strcmp(verb, "TRIG") == 0) {
        if (!arg) {
            snprintf(resp, resp_sz, "+TRIG:%d", g_trigger_only ? 1 : 0);
        } else if (*arg == '0' || *arg == '1') {
            g_trigger_only = (*arg == '1');
            snprintf(resp, resp_sz, "OK");
        } else {
            snprintf(resp, resp_sz, "ERR:用法 AT+TRIG=0|1 (发 AT+TRIG? 看每个值的含义)");
        }
    } else if (strcmp(verb, "PAD") == 0) {
        if (!arg) {
            snprintf(resp, resp_sz, "+PAD:%d", g_pad_profile);
        } else {
            const int k = atoi(arg);
            if (*arg == '\0' || k < 0 || k > 3) {
                snprintf(resp, resp_sz, "ERR:用法 AT+PAD=0..3 (发 AT+PAD? 看每个值的含义)");
            } else {
                pad_apply_profile(k);
                snprintf(resp, resp_sz, "OK");
            }
        }
    } else if (strcmp(verb, "INFO") == 0) {
        // P5.78: AT+INFO 第二行的 +CFG 半行 —— LOG/IMG/TRIG/PAD 只有主固件知道, 这里补齐 (PLATE 由 node_link 自己接)
        snprintf(resp, resp_sz, "LOG=%d,IMG=%d,TRIG=%d,PAD=%d",
                 g_verbose_mode ? 1 : 0, g_img_mode_want, g_trigger_only ? 1 : 0, g_pad_profile);
    }
    // P5.74: HELP / AT+XXX? 不再走这里 —— 完整命令表和取值含义都在 node_link.c (多行, 回调只有一行装不下)
}

extern "C" void app_main(void) {
    apply_log_levels();   // P5.76: 第一件事就定档 —— 简单模式(默认)之后 ESP_LOG 一个字都不出
    if (!g_verbose_mode) {
        plain_out("=== LPRNet on ESP32-S3 —— 固件 P5.80 (简单日志: 只打结果块/未识别原因; 想看详细就发 AT+LOG=1 或长按 BOOT) ===");
    }
    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.80 ===");
    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.80 才说明烧进去的是新固件", __DATE__, __TIME__);
    ESP_LOGI(TAG, "    P5.76: **日志两档** —— 简单(默认)只打一个结果块(车牌号码/颜色/置信度/帧号/位置); 详细(AT+LOG=1 或长按 BOOT)才有 ROI 行/候选表/掩码/警告");
    ESP_LOGI(TAG, "    P5.77: **同一帧不再打两遍** —— 每帧自动上行的 $PLATE 只走真节点口, 电脑口只看结果块(AT+TEST/AT+PUSH 例外, 那是人主动要看线上格式); **没拍出来也给原因** —— 被 AT+RUN 点名的那一帧没出结果时补一个\"未识别\"块(车牌号码 - + 一行原因 + 帧号), 连续模式不打");
    ESP_LOGI(TAG, "    P5.78: **AT+INFO 一眼看全** —— 第1行 +INFO 是版本/时长/统计, 第2行 +CFG 把每条带取值的命令当前值列一遍: LOG/IMG/TRIG/PAD/PLATE");
    ESP_LOGI(TAG, "    P5.79: **只读命令不看控制权** —— AT+HELP / AT+INFO / AT+XXX? 任何口、任何模式都能发 (AT+CTRL 和电脑口的 AT+IMG 照旧放行)");
    ESP_LOGI(TAG, "    P5.80: **节点链路改走真串口** —— 节点口从控制台切到 UART1 (TX=GPIO47 / RX=GPIO48, 板上丝印 SDA/SCL 的那两个脚, 115200 8N1); 每帧自动上行的 $PLATE 同步抄一份 [镜像] 到电脑口(没接节点板也能验证); 电脑要发命令仍是先 AT+CTRL=PC");
    ESP_LOGI(TAG, "    P5.75: AT+HELP 命令表前后各加一行 \"-----\" 分隔; 删掉 AT+MIRROR 命令 —— 镜像没有要关掉的场景, 一直开着");
    ESP_LOGI(TAG, "    P5.74: IMG 档位重排: 0=关(默认) 1=干净预览 2=预览+绿框 3=模型输入块94x24");
    ESP_LOGI(TAG, "    P5.74: AT+RUN 不回 OK、也不另发一行 —— 它的回复就是那一帧的 $PLATE,<车牌>,<置信度>,<帧号>");
    ESP_LOGI(TAG, "    P5.74: 图片档位不再跟控制权挂钩 —— **电脑口在正常模式下也能设 AT+IMG**; 档位非 0 时每条 $PLATE 尾巴带 img=<档位> 当提醒");
    ESP_LOGI(TAG, "    P5.74: **AT 回复规范统一** —— 设置类回 OK(失败 ERR:<原因>); 查询类只回 +XXX:<当前值>, 不带多余解释; 新增 AT+XXX? 看该命令每个取值的含义");
    ESP_LOGI(TAG, "    P5.74: AT+PLATE? 改名 AT+PLATE (? 现在专用于\"看含义\"); AT+IMG/AT+TRIG/AT+LOG 查询只回纯值");
    ESP_LOGI(TAG, "    P5.73: **切换控制权只回 OK** —— AT+CTRL 设置成功就是一行 OK, 不再打印任何日志(链路层那行重复的和主固件那行副作用提示都删了)");
    ESP_LOGI(TAG, "    P5.71: **命令回复规范化** —— 带 = 的设置命令成功一律回 OK(原来回的是值), 不带 = 的查询才回当前值; 图片档位不再暴露内部的[生效/设置]两个数");
    ESP_LOGI(TAG, "    P5.70: **镜像** —— 节点口的对话(收到的命令 + 发出的 $PLATE/回复)抄一份到电脑口, 调试时看得见; 接真节点(UART1)自动开 (P5.75 起删掉 AT+MIRROR 开关, 一直开着)");
    ESP_LOGI(TAG, "    P5.69: **控制权模式** —— 一个开关 AT+CTRL 决定「谁能发命令」: 正常模式(上电默认)节点是主人/电脑只读, 调试模式电脑是主人/节点只读");
    ESP_LOGI(TAG, "           电脑要发命令先解锁: AT+CTRL=PC (图片输出不跟控制权挂钩, 电脑口在两种模式下都能设 AT+IMG)");
    ESP_LOGI(TAG, "    P5.69: AT+MODE 改名 AT+TRIG=<0|1> (1=只听 AT+RUN, 默认); BOOT 按键改成只换设置, 不再顺手拍一帧");
    ESP_LOGI(TAG, "    P5.68: **摄像头接回节点** —— 识别结果不再只打日志, 还以 $PLATE,<车牌>,<置信度>,<帧号> 送给节点; 部署时节点检测到车 -> 发 AT+RUN -> 收 $PLATE");
    ESP_LOGW(TAG, "默认工作模式: **只听触发** —— 开机不会自动拍照; 发 AT+RUN 拍一帧, 发 AT+TRIG=0 恢复连续自动识别");
    ESP_LOGI(TAG, "    P5.66: **加第二个模型专治省字** —— 主模型吃 94x24, 省字只剩 13x22 像素 (原图里有 44x138); 于是另训一个 31 类省字分类器, 直接从原图四边形抠省字格 (32x64) 喂它");
    ESP_LOGI(TAG, "           为什么原来读不对: replay 微调集 760/800 是皖牌 -> 模型学会「省字看不清就报皖」; 实测同一张裁块人眼读京、模型 93~99%% 读皖");
    ESP_LOGI(TAG, "           新模型 PC 端 4 折交叉验证 (245 张真实省字, 每张都由没见过它的模型评): top-1 94.7%%, 复核置信 >=%.0f%% 的那些 97.2%% (覆盖 89%%); 低于门槛就保留主模型首字", PROV_CONF_MIN * 100.0f);
    ESP_LOGI(TAG, "    P5.61: 长边收边整段删除 + 左留白 净0%%->净+2%% + 上下留白 净5%%->净11%%");
    ESP_LOGI(TAG, "    P5.60: **蓝牌判据换成 (b-r)>40 且 b>120; 比例窗 2.0~6.5 -> 2.2~4.2; 严格档贴边也照用; 裁剪块的[按亮度跨度拉伸]换成[整数饱和度 x2.0]**");
    ESP_LOGI(TAG, "           依据: 用户新拍 171 张原始帧(京Q06666/豫FSQ818/豫A8F8Q8), PC 端到端复算 整串全对 28/171 -> 114/171");
    ESP_LOGI(TAG, "           拆开看: 只换蓝牌判据 28->96 | 再换裁剪增强 +12 | 贴边不再整帧丢 +6 | 比例窗 +1");
    ESP_LOGI(TAG, "    P5.36~P5.59: 长边收边(按实测蓝色边界把取样框左端收窄) —— **P5.61 已整段删除**(0 帧有帮助 / 52 帧帮倒忙)");
    ESP_LOGI(TAG, "    P5.40: 取样几何(长边左右留白)做成 4 档, 三击 BOOT 循环切: 档0 左净0%%/右净4%% -> 档1 左+2%%/右4%% -> 档2 左0%%/右+7%% -> 档3 左-4%%/右+7%%");
    ESP_LOGI(TAG, "    P5.42: 曾把默认改档3(左右各 +7%%) —— 已回退, 见下");
    ESP_LOGI(TAG, "    P5.43: **默认档 3 -> 2 (左净0%% / 右净+7%%)** —— 档3 的左右各+7%% 会把[左边凭空多认一个省字]放回来: 61 帧里 38 帧带多余省字, 京Q06666 一帧没全对");
    ESP_LOGI(TAG, "    P5.44: **换模型** —— 用你的实拍素材微调 + mse 校准。留出33张 int8 39.4%% -> 75.8%%(翻倍); 代价: 皖牌验证集 83.5%% -> 69.0%%");
    ESP_LOGI(TAG, "           想回退: 烧 p544alt(P5.44-alt, 只换校准) 或 p543(P5.43, 原样)");
    ESP_LOGI(TAG, "    P5.46: **形状不再当裁判** —— 严格档挑不出候选时, 兜底档『面积够大就收, 比例/填充一概不看』(治屏幕反光把蓝底打散), 日志打[兜底]");
    ESP_LOGI(TAG, "    P5.47: **贴边不再一票否决** —— 得分最高的候选贴到画面边缘时, 先在同一档里换一个不贴边的候选; 真没有替补才拒绝");
    ESP_LOGI(TAG, "    P5.48: **兜底档贴边照用** —— 兜底档的候选贴到画面边缘时不再换替补(实测会换成 色格300 比例15.86 的细缝, 更糟), 直接用, 结果交给闸门判");
    ESP_LOGI(TAG, "           严格档(形状像车牌)贴边仍旧 先换替补/没有就拒绝; 依据: 读对的 #133(542x212 占屏38.6%%) 就是框比车牌大一圈也照读");
    ESP_LOGI(TAG, "           为什么: 反光会把蓝掩码漏到画面边缘, 于是得分最高的那块是贴边杂块, 而车牌完整地在画面中间 —— 那时整帧丢掉是白丢");
    ESP_LOGI(TAG, "           代价靠『结果闸门』兜底: 结果必须先过车牌语法(7~8位, 省字开头)才当 RESULT 上报, 否则只打一行『疑似误检』(旧固件是把乱码也当结果报)");
    ESP_LOGI(TAG, "    P5.49: **亮度下限 46 -> 120** —— 屏幕那层深蓝底色和车牌蓝的色相/饱和度都重叠, 唯一分得开的是亮度(车牌蓝底最亮通道约150~200, 屏幕底色只有60~90)");
    ESP_LOGI(TAG, "           依据(2026-10-01 用户 20 张难帧, PC 复算): 只把 V 从 46 抬到 120, 『像车牌的框』0/20 -> 15/20, 巨框(占屏>45%%) 18/20 -> 0/20; 饱和度一个字没改");
    ESP_LOGI(TAG, "           代价: 很暗的车牌(B 通道掉到 ~100)会被误杀 -> 该帧打印『画面里没有一块车牌色』直接跳过, 不会出巨框");
    ESP_LOGI(TAG, "    P5.57(已在 P5.59 删掉): **拆掉每帧诊断 + 影子对照** —— 曾每帧多裁 2 块(蓝带两端收 / 只收左端)各跑一次模型, 只打一行 `影子:` 日志; 结论拿到后连影子一起去掉, 换回速度");
    ESP_LOGI(TAG, "    P5.59(P5.61 已整段删除): 长边收边曾定案\"只收左端\" —— 复算证明它 0 帧有帮助/52 帧帮倒忙, 代码已删; 详见文档第四十五节");
    ESP_LOGI(TAG, "    P5.56/P5.58: **占屏下限 %.1f%%** —— 上下限成对: 45%% 治整屏背景, %.1f%% 治碎块。原来底线只有 0.2%%, 84x16 / 76x16 px 这种横条两条路都能溜过去(比例+填充居然都合格), 喂给模型必吐乱码", ROI_MIN_AREA_PCT, ROI_MIN_AREA_PCT);
    ESP_LOGI(TAG, "    P5.49: **候选加 %.0f%% 占屏上限** —— 严格档和兜底档都不收超上限的块(实测真车牌本体 <=32%%, 被反光污染的块 52%%~90%%); 淘汰时日志打『占屏过大(疑似反光/背景连成一片)』", ROI_MAX_AREA_PCT);
    ESP_LOGI(TAG, "    P5.41: BOOT 的 ISR 加了去抖(50ms) —— 上一轮三击被数成 2 下(档位没切)或 6 下, 就是触点回弹");
    ESP_LOGI(TAG, "    BOOT: 单击(<1s)=切图片模式 | 双击=复位图片输出 | 三击=循环切取样几何档 | 长按(>=2s)=详细模式开关");
    ESP_LOGI(TAG, "          P5.69 起按键只换设置, 不再顺手拍一帧 —— 想立刻看效果: 切 AT+CTRL=PC 再发 AT+RUN (或 AT+TRIG=0 连续跑)");
    ESP_LOGI(TAG, "    连续模式: 每帧 2 行(#n 指标行 + #n RESULT, 结果带置信度); 结果可疑时自动补打一行诊断");
    ESP_LOGI(TAG, "    完整诊断(掩码+候选表+覆盖率剖面): 长按 BOOT >=2s 走一轮; 自动诊断开关 = main.cpp 的 AUTO_DIAG_ON_SUSPECT");
    ESP_LOGI(TAG, "    串口图片: $IMG,<len> + JPEG + CRC32(大端4B) + $END  -> BY串口助手选「二进制帧」, 波特率 %d", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    ESP_LOGI(TAG, "              发图走电脑那条口(不占节点线); 电脑口在正常/调试模式下都能设 AT+IMG, 默认 0(关); 发图时 $PLATE 尾巴带 img=");
    ESP_LOGI(TAG, "    图片输出一旦不发图: 按一下 BOOT 会自动复位(关编码器再重开), 不必重启");

    // ---- 1. 建模型 (直接从 flash rodata 加载) ----
    int64_t t0 = esp_timer_get_time();
    dl::Model *model = new dl::Model((const char *)model_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    ESP_LOGI(TAG, "model loaded in %.1f ms", (esp_timer_get_time() - t0) / 1000.0f);

    model->profile_memory();

    dl::TensorBase *model_input = model->get_inputs().begin()->second;
    dl::TensorBase *model_output = model->get_outputs().begin()->second;
    ESP_LOGI(TAG, "input  shape=%s dtype=%s exponent=%d",
             dl::vector_to_string(model_input->shape).c_str(),
             model_input->get_dtype_string(), model_input->exponent);
    ESP_LOGI(TAG, "output shape=%s dtype=%s exponent=%d",
             dl::vector_to_string(model_output->shape).c_str(),
             model_output->get_dtype_string(), model_output->exponent);

    // 布局自检: 输入必须是 NHWC 的 [1, IMG_H, IMG_W, 3]
    if (model_input->shape.size() != 4 || model_input->shape[1] != IMG_H ||
        model_input->shape[2] != IMG_W || model_input->shape[3] != 3) {
        ESP_LOGE(TAG, "模型输入形状不是 [1,%d,%d,3] (NHWC), 而是 %s —— 检查导出/喂数布局",
                 IMG_H, IMG_W, dl::vector_to_string(model_input->shape).c_str());
    }

    // ---- 2. 归一化 LUT + 解码用的 float 输出张量 ----
    int8_t *norm_lut = (int8_t *)malloc(3 * 256);
    build_norm_lut(norm_lut, model_input->exponent);
    build_inv_lut(norm_lut);   // P5.16: 反查表, 用来把输入块还原成图片
    ESP_LOGI(TAG, "norm lut: mean=%.1f std=%.1f lut[0]=%d lut[128]=%d lut[255]=%d",
             NORM_MEAN, NORM_STD, norm_lut[0], norm_lut[128], norm_lut[255]);

    dl::TensorBase *output_float =
        new dl::TensorBase(model_output->shape, nullptr, 0, dl::DATA_TYPE_FLOAT);

    // ---- P5.66: 第二个模型 —— 省字复核 (输入 32x64, 从原图四边形抠省字, 不走 94x24) ----
    {
        const int64_t tp = esp_timer_get_time();
        g_prov_model = new dl::Model((const char *)prov_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        g_prov_input = g_prov_model->get_inputs().begin()->second;
        dl::TensorBase *prov_output = g_prov_model->get_outputs().begin()->second;
        ESP_LOGI(TAG, "省字模型 loaded in %.1f ms | input %s %s exp=%d | output %s %s exp=%d",
                 (esp_timer_get_time() - tp) / 1000.0f,
                 dl::vector_to_string(g_prov_input->shape).c_str(), g_prov_input->get_dtype_string(),
                 g_prov_input->exponent, dl::vector_to_string(prov_output->shape).c_str(),
                 prov_output->get_dtype_string(), prov_output->exponent);
        if (g_prov_input->shape.size() != 4 || g_prov_input->shape[1] != PROV_H ||
            g_prov_input->shape[2] != PROV_W || g_prov_input->shape[3] != 3) {
            ESP_LOGE(TAG, "省字模型输入形状不是 [1,%d,%d,3] (NHWC), 而是 %s —— 关掉这条支路",
                     PROV_H, PROV_W, dl::vector_to_string(g_prov_input->shape).c_str());
            g_prov_model = nullptr;
        } else if (prov_output->shape.size() != 2 || prov_output->shape[1] != PROV_CLASS) {
            ESP_LOGE(TAG, "省字模型输出形状不是 [1,%d], 而是 %s —— 关掉这条支路",
                     PROV_CLASS, dl::vector_to_string(prov_output->shape).c_str());
            g_prov_model = nullptr;
        } else {
            g_prov_lut = (int8_t *)malloc(3 * 256);
            // P5.67: 发图用的 patch 原色缓冲 (PSRAM); 分不到只是不发那张诊断图, 不影响复核本身
            g_prov_rgb = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)PROV_W * PROV_H * 3,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            build_prov_lut(g_prov_lut, g_prov_input->exponent);
            g_prov_out = new dl::TensorBase(prov_output->shape, nullptr, 0, dl::DATA_TYPE_FLOAT);
            ESP_LOGI(TAG, "省字 LUT: lut[0]=%d lut[128]=%d lut[255]=%d (mean %.1f std %.1f) | 采纳门槛 %.0f%%",
                     g_prov_lut[0], g_prov_lut[128], g_prov_lut[255],
                     PROV_NORM_MEAN, PROV_NORM_STD, PROV_CONF_MIN * 100.0f);
        }
    }

    // P5.50: 开机无条件跑一次内嵌样张自检 (原来只在「摄像头起不来」时才跑)。
    //   两张图: 老样张当回归基线; 探针是 PC 端 float 读作 京Q06666 的那一帧裁块。
    //   板端若也读出 京Q06666 -> 推理链路没问题, 问题在取样/发图; 板端若读出 京Q粤 -> 推理本身有问题。
    run_probe_selfcheck(model, model_input, output_float, test_input_bin, "旧样张 沪AMS087");
    run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针 板端发回的裁块");
    // P5.51: 同一条数据, 换成"和实时链路一模一样的直写 int8"再跑一次
    run_probe_direct_int8(model, model_input, output_float, probe_crop_bin, "探针/直写int8");
    run_probe_direct_int8(model, model_input, output_float, test_input_bin, "旧样张/直写int8");

    // ---- 2.5) 节点链路 (摄像头 -> 节点单片机) ----
    //   P5.80: 节点口已经是 UART1 (TX=GPIO47 / RX=GPIO48, 丝印 SDA/SCL 那两个脚, 115200 8N1)。
    //   现在手上没有节点板: 那根线上发出去的内容靠电脑口的 [镜像] 行核对; 电脑要发命令先 AT+CTRL=PC 抢控制权。
    //   接节点时: 摄像头 TX(47) -> 节点 USART3 RX(PB11), 摄像头 RX(48) -> 节点 USART3 TX(PB10), 共地。
    node_link_init("P5.80");
    node_link_set_extra_handler(nl_extra_cmd);
    img_mode_apply();   // P5.74: 上电生效档 = 默认档 0(关); 电脑口随时 AT+IMG=<档> 就能开图

    // ---- 3. BOOT 按钮 (GPIO0, 低电平按下) ----
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BTN_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;   // 按下/松手都要中断: 按下锁存, 松手量时长
    gpio_config(&io);
    // 中断服务已装过就忽略 (ESP_ERR_INVALID_STATE), 不算错误
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service 失败: %s (BOOT 按钮改为只在两轮之间轮询)", esp_err_to_name(isr_ret));
    } else {
        gpio_isr_handler_add((gpio_num_t)BOOT_BTN_PIN, boot_btn_isr, nullptr);
    }

    // ---- 4. 摄像头 ----
    bool camera_ok = (camera_start() == ESP_OK);
    if (camera_ok) {
        camera_discard_frames(CAM_WARMUP_FRAMES, CAM_WARMUP_DELAY_MS);   // 等 AE/AGC 稳定, 否则头几帧偏暗
        ESP_LOGI(TAG, "摄像头就绪: VGA RGB565, XCLK 20MHz, fb_count=2");
        if (!g_verbose_mode) plain_out("摄像头就绪: VGA RGB565 (简单日志; AT+HELP 看命令表)");
        // P5.52: 开机自检时探针读对, 摄像头一起来就变错 -> 说明是运行时内存/环境被搅了, 不是数据问题
        run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针/摄像头已开第1次");
        run_probe_selfcheck(model, model_input, output_float, probe_crop_bin, "探针/摄像头已开第2次");
        ESP_LOGI(TAG, "触发方式: %s —— 发 AT+RUN 立刻拍一帧, 发 AT+TRIG=0 回到连续自动识别(周期 %u ms)",
                 g_trigger_only ? "**只听触发**(默认, 开机不会自动拍)" : "连续自动识别",
                 (unsigned)AUTO_PERIOD_MS);
        ESP_LOGI(TAG, "图片输出: 档 %d (0=关 1=干净预览 2=预览+绿框 3=模型输入块94x24; 非 0 时 $PLATE 尾巴带 img=); 详细模式 %s; 取样几何档 %d (左净 %+.0f%% 右净 %+.0f%%)",
                 (int)g_img_mode, g_verbose_mode ? "开" : "关", g_pad_profile,
                 (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);
        ESP_LOGI(TAG, "控制权: %s —— 电脑要发命令先 AT+CTRL=PC (AT+HELP 看命令表)",
                 node_link_ctrl_is_pc() ? "调试模式(电脑是主人)" : "正常模式(节点是主人, 电脑只读)");
    } else {
        ESP_LOGE(TAG, "摄像头不可用, 先跑一次内嵌样张自检; 之后每 5s 重试摄像头");
        if (!g_verbose_mode) plain_out("摄像头不可用: 先跑内嵌样张自检, 之后每 5s 重试");
        run_embedded_selfcheck(model, model_input, output_float);
    }

    // ---- 5. 主循环: 按钮 / 定时触发识别 ----
    TickType_t next_auto = 0;          // 0 => 第一次进连续模式时立刻跑 (默认「只听触发」, 开机不会自动跑)
    TickType_t next_cam_retry = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();

        // P5.68: 收"节点"发来的 AT 命令 (非阻塞; 一轮识别 ~0.9s, 命令最快下一轮才被处理)
        node_link_poll();

        // P5.73: 切换控制权只回一行 OK, 不打任何日志 (要状态就发 AT+CTRL 查询)。
        // P5.74: 图片档位不再跟着控制权变 —— 这里只是把事件消费掉, 顺手同步一次生效档/提醒。
        if (node_link_take_ctrl_event()) img_mode_apply();

        // P5.68: 节点发来的"拍一张"(AT+RUN) —— 在这里执行, 因为模型句柄/摄像头状态都是 app_main 的局部量
        if (g_run_request) {
            g_run_request = false;
            if (camera_ok) {
                recognize_frame(model, model_input, output_float, norm_lut, "节点触发(AT+RUN)", g_verbose_mode, true);
                // 跳过推理 / 认不出的兜底补行由 recognize_frame() 统一负责, 这里不用再管。
                next_auto = xTaskGetTickCount() + pdMS_TO_TICKS(AUTO_PERIOD_MS);
            } else {
                ESP_LOGW(TAG, "AT+RUN: 摄像头当前不可用, 直接回一条空结果");
                print_fail_block("摄像头当前不可用 (开机自检没通过, 每 5s 自动重试)", 0);
                node_link_report_plate("", false, 0, 0);
            }
            // P5.74: 节点口的回复就是 recognize_frame() 刚发出去的那行 $PLATE (档位非 0 时尾巴带 img=);
            //   P5.77: 电脑口看的是结果块, 没出结果时就是上面那条"未识别"块。
        }

        if (camera_ok) {
            // P5.15: 单击(<1s) = 循环切换图片输出模式; 长按(>=2s) = 详细模式开关 (P5.31 起改成开关, 不再是"一帧")。
                    // P5.35/P5.61: 双击 = 复位图片输出 (原来是 1~2s 中按, 太容易误触 —— 用户实测反馈)。
            //        中间 1~2s 退回"空档": 想单击但手抖按久了、想长按但没按够, 都当作没按。
            // 以中断锁存为主; 万一 ISR 装不上, 电平轮询这条老路仍然兜底 (那条路量不出时长, 一律当短按)。
            const bool btn_down = (boot_btn_latched || gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0);
            if (btn_down) {
                boot_btn_latched = false;
                const int32_t press_ms = boot_press_ms;
                // P5.30: 原来这里是"看一眼就走" —— 主循环每轮要跑一整次识别 (~1s), 等它转回来问按键时,
                //   按键才按了 0~1s, 于是永远判成短按 (旧日志里一次 "BOOT 长按" 都没有)。
                //   改成当场等: 一直等到松手; 或者等到够 2s 就直接判长按(不松手也有反馈), 手感才对。
                int32_t dur_ms = 0;
                if (press_ms != 0) {
                    if (boot_release_ms != 0) {
                        dur_ms = boot_release_ms - press_ms;       // 松手边沿已经记到了, 直接用最准
                    } else {
                        while (true) {
                            dur_ms = (int32_t)(esp_timer_get_time() / 1000) - press_ms;
                            if (dur_ms >= 2000) break;                                 // 够 2s: 判长按
                            if (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) != 0) break;   // 松手了: 按真实时长判
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    }
                }
                const bool long_press = (press_ms != 0) && (dur_ms >= 2000);
                const bool short_press = (press_ms == 0) || (dur_ms < 1000);   // press_ms==0 = 轮询兜底, 量不出时长, 一律当短按
                    // P5.35: 单击要等一下再执行 —— 看 BOOT_DBLCLICK_MS 内还有没有第二下, 有就是双击(=复位图片输出), 没有才是单击。
                //   代价: 单击切图片模式会晚 400ms 才拍那一帧; 换来的是不会再把"手抖按了两下"误判成两次切模式。
                // P5.36: 先看 ISR 数出来的"这一串按了几下" —— 设备忙的时候两下早就都按完了, 只有这个数靠得住。
                //   在这里就把账结掉(而不是等收尾), 免得这一串的计数污染下一次; 之后窗口里再来的那一下由 latched 兜住。
                // P5.39: 要认三击, 就得把窗口期等完再一次性清账 —— 否则第一下读到的永远是 burst_n=1,
                //   后两下发生在窗口期里, 只能看出"至少两下", 分不出两下还是三下。
                bool multi = (boot_burst_n >= 2);
                if (!multi && short_press && press_ms != 0) {
                    const int64_t dl = (int64_t)(esp_timer_get_time() / 1000) + BOOT_DBLCLICK_MS;
                    while ((int64_t)(esp_timer_get_time() / 1000) < dl) {
                        if (boot_btn_latched || gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0) { multi = true; break; }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    if (multi) {
                        // 等这一串彻底松手(用户可能还在按第三下); 顺手把按下/松手记账清掉, 免得又被当成单击
                        while (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                        boot_press_ms = 0;
                        boot_release_ms = 0;
                        boot_btn_latched = false;
                    }
                }
                const uint8_t burst_n = boot_burst_n;   // 窗口期过完再读, 三下都在里面
                boot_burst_n = 0;
                const bool triple_click = (burst_n >= 3);
                const bool dbl_click = (burst_n == 2);
                ESP_LOGI(TAG, "BOOT 手势: 这一串按了 %u 下, 最后一下时长 %d ms -> %s", (unsigned)burst_n, (int)dur_ms,
                         triple_click ? "三击" : (dbl_click ? "双击" : (long_press ? "长按" : (short_press ? "单击" : "空档"))));
                if (long_press) {
                    g_verbose_mode = !g_verbose_mode;   // P5.31: 长按 = 详细模式开关
                    apply_log_levels();                 // P5.76: 日志档位跟着切
                    img_tx_reset("BOOT 长按");   // P5.20: 顺手复位图片输出 —— 卡住时按一下就能救回来
                    ESP_LOGW(TAG, "详细模式: %s —— 之后每一帧%s完整诊断, 再长按一次切换 (P5.69 起按键只换设置, 不再顺手拍一帧)",
                             g_verbose_mode ? "开" : "关", g_verbose_mode ? "都带" : "都不带");
                } else if (dbl_click) {
                    // P5.61: 双击原来 = "长边收边"开关, 收边已整段删除; 现在改成 **图片输出全复位 + 打一张发图统计** ——
                    //   串口图卡住时双击一下就能救回来, 而且不改图片模式 (不会因为救卡而多刷几帧)。
                    // ⚠ 这一支必须排在 short_press 前面: dbl_click 为真时 short_press 必然也为真,
                    //   放在后面编译器会判定"永远走不到"而整块删掉 (第一次写反就是这么踩的)。
                    img_tx_reset("BOOT 双击");
                    ESP_LOGW(TAG, "发图统计: 累计成功 %u 帧 / %u KB; 连续失败 %u 次 (最后原因: %s)",
                             (unsigned)g_tx_ok, (unsigned)(g_tx_bytes / 1024), (unsigned)g_tx_fail,
                             g_tx_fail_why ? g_tx_fail_why : "-");
                } else if (triple_click) {
                    // P5.40: 三击 = 循环切"取样几何档" 0 -> 1 -> 2 -> 3 -> 0 (四档定义见 k_pad_profiles)。
                    //   ⚠ 这一支必须排在 short_press 前面 (三击时 short_press 必然也为真), 否则被整块删掉。
                    static const char *profile_name[4] = {
                        "档0 左净0%/右净4% (P5.32~P5.39 老几何)",
                        "档1 左净+2%/右净4% (P5.38, 真机330~420px 83%)",
                        "档2 左净0%/右净+7% (P5.43 默认)",
                        "档3 左净-4%/右净+7% (往框里收)"};
                    pad_apply_profile((g_pad_profile + 1) & 3);
                    ESP_LOGW(TAG, "取样几何 -> %s: 左端留白净 %+.0f%%, 右端净 %+.0f%% (再按三下切下一档)",
                             profile_name[g_pad_profile],
                             (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);
                    img_tx_reset("BOOT 三击");
                } else if (short_press) {
                    img_tx_reset("BOOT 短按");   // P5.20: 同理, 不用再重启单片机
                    g_img_mode_want = (g_img_mode_want + 1) & 3;
                    img_mode_apply();
                    static const char *mode_name[4] = {
                        "关 (不发图 —— 默认档, 接真节点用这档)",
                        "干净预览图 (320x240, 画面/采数据)",
                        "预览图 + 绿框 (看框套得准不准)",
                        "模型输入块 (94x24, 训练素材 —— 和推理输入逐像素一致)"};
                    ESP_LOGW(TAG, "图片输出设置: %s%s", mode_name[g_img_mode_want & 3],
                             (g_img_mode_want == 0) ? "" : "  (非 0 时每条 $PLATE 尾巴会带 img= 提醒)");
                } else {
                    ESP_LOGW(TAG, "按键 %d ms 落在 1~2s 空档里, 当作没按 (单击<1s=切图片模式, 双击=复位图片输出, 三击=循环切取样几何档, 长按>=2s=详细模式开关)", (int)dur_ms);
                }
                boot_press_ms = 0;
                boot_release_ms = 0;
                while (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));   // 等松手, 免得一次长按连跑好几轮
                }
                boot_btn_latched = false;   // 松手过程中的抖动边沿一并清掉
                next_auto = xTaskGetTickCount() + pdMS_TO_TICKS(AUTO_PERIOD_MS);
            } else if (!g_trigger_only && AUTO_PERIOD_MS > 0 && now >= next_auto) {
                recognize_frame(model, model_input, output_float, norm_lut,
                               (g_verbose_mode ? "定时(连续/详细开)" : "定时(连续)"), g_verbose_mode, false);
                next_auto = now + pdMS_TO_TICKS(AUTO_PERIOD_MS);
            }
        } else if (now >= next_cam_retry) {
            esp_camera_deinit();
            camera_ok = (camera_start() == ESP_OK);
            if (camera_ok) {
                camera_discard_frames(CAM_WARMUP_FRAMES, CAM_WARMUP_DELAY_MS);
                ESP_LOGI(TAG, "摄像头重试成功");
                next_auto = 0;
            } else {
                next_cam_retry = now + pdMS_TO_TICKS(5000);
            }
        }

        // P5.27: 原来是 50 ms。这一句只是"没事时别空转"的礼貌延时, 但每圈都会实打实
        //   推迟下一帧的触发, 一帧就是 50 ms 的净损失(约 4%)。降到 10 ms, 空闲时依旧让出 CPU。
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
