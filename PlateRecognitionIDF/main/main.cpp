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

static const char *TAG = "plate";

// 由 CMake target_add_aligned_binary_data 嵌入到 flash rodata
extern const uint8_t model_espdl[] asm("_binary_lprnet_s3_espdl_start");
extern const uint8_t test_input_bin[] asm("_binary_test_input_bin_start");

static const int IMG_H = 24;
static const int IMG_W = 94;
static const int NUM_CLASS = 68;   // 65 字符 + blank, blank = 67
static const int TIME_STEPS = 18;

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

// BOOT 按钮用中断锁存 (P5.3): 连续识别时每轮要跑 ~0.9s, 如果只在两轮之间轮询电平,
// 用户"短按一下"很容易正好落在忙的那 0.9s 里被漏掉 (以前周期 5s、空闲 3.3s, 所以没暴露)。
// 用边沿中断把"按过"这件事记下来, 循环下一圈再消费 —— 哪怕只按 10ms 也不会丢。
// P5.15: 用 ANYEDGE 顺便量按下时长, 区分短按(绿框开关) / 长按(详细模式)
static volatile bool boot_btn_latched = false;
static volatile int32_t boot_press_ms = 0;       // 按下时刻 (ms, 32 位: ISR 与主循环共享也读不坏)
static volatile int32_t boot_release_ms = 0;     // 松手时刻 (ms), 0 = 还按着
static void IRAM_ATTR boot_btn_isr(void *arg) {
    (void)arg;
    if (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0) {   // 按下
        boot_press_ms = (int32_t)(esp_timer_get_time() / 1000);
        boot_release_ms = 0;
        boot_btn_latched = true;
    } else {                                               // 松手
        boot_release_ms = (int32_t)(esp_timer_get_time() / 1000);
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

static void build_norm_lut(int8_t *lut, int exponent) {
    const float inv_scale = 1.0f / DL_SCALE(exponent);   // exponent=-7 -> 128
    for (int c = 0; c < 3; c++) {
        for (int v = 0; v < 256; v++) {
            // 必须与 esp-dl 的 quantize<int8_t>() 逐位一致, 否则和已验证的板端 float 喂数链路差 1 个 LSB:
            //   dl/tensor/src/dl_tensor_base.cpp:8   quantize = tool::round(input * inv_scale) 然后 clip
            //   dl/tool/src/dl_tool.cpp:69-88        S3 的 tool::round = (int)floorf(value + 0.5f)  (半值向 +inf)
            //   (P4 走的是 round_half_even, 与这里不同; 本工程只跑 S3)
            // ⚠ 不要用 lroundf()/round(): 它们是"半值远离 0", 对 v<=127 会整体差 1。
            //   本模型下 (v-127.5)/128*128 恰好等于 v-127.5, 故结果就是 clamp(v-127)。
            int q = (int)floorf((v - NORM_MEAN) / NORM_STD * inv_scale + 0.5f);
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            lut[c * 256 + v] = (int8_t)q;
        }
    }
}

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
    ESP_LOGI(TAG, "GC2145 寄存器: 曝光目标(页1 0x13)=0x%02x | AEC使能(0xb6)=0x%02x | 自动开关(页0 0x82)=0x%02x",
             tgt, gc_rd(s, GC_PAGE_AEC, 0xb6) & 0xff, gc_rd(s, 0, 0x82) & 0xff);
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
static const float ROI_RATIO_LO = 2.0f;           // auto_crop_predict.py: ratio < 2.0 丢弃
static const float ROI_RATIO_HI = 6.5f;           // auto_crop_predict.py: ratio > 6.5 丢弃
static const float ROI_RATIO_IDEAL = 3.4f;        // auto_crop_predict.py: 打分基准
// P5.2 有效填充率下限。有效填充率 = 能摆正 => 旋转矩形填充率(与倾角无关); 否则 => 轴对齐外接框密度。
// 依据 (2026-09-28 新固件日志, 10 次会话): 真车牌 65%~90% 全对/接近; 散块噪块 35%~54% 全乱。
// 离线自检 tools/roi_geom_selftest.ps1: 直立 88%, 倾斜 20/25/33 度 85/85/84%。
// ⚠ 不能拿轴对齐密度一刀切: 倾斜车牌的外接框密度只有 40%~47% (离线 F/G/H), 那样会误杀真车牌。
static const float ROI_MIN_FILL = 0.60f;
static const float ROI_TRIM_X = 0.01f;            // auto_crop_predict.py: 左右各去 1%
static const float ROI_TRIM_Y = 0.03f;            // auto_crop_predict.py: 上下各去 3%

// P5.13: 采样的"外扩留白"。训练素材里字符是不顶边的(见 data/official_val 的 94x24 图),
//   而我们的框是"蓝区紧贴边" + 再去 1%/3% 边 => 字符在 94 宽的张量里被拉宽约 14%,
//   模型会把最后一个字挤掉(京Q06666 -> 京Q0666)。
//   实测(同一块牌 35 帧, 浮点 ONNX 离线跑):
//     外扩 0%  -> 2/35 正确(其中 30 帧都少最后一个 6)
//     外扩 4%  -> 10/35
//     外扩 8%  -> 28/35   <= 取这个
//     外扩 10% -> 18/35, 12% -> 0/35(开始把画面里的东西也框进来, 多认字)
static const float ROI_CROP_PAD = 0.08f;

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


// P5.15/P5.16: 图片输出模式 —— 短按 BOOT 循环切换。默认 0。
//   0 = 干净预览图 (320x240, 采数据/日常看画面)
//   1 = 预览图 + ROI 绿框 (专门用来看"框套得准不准")
//   2 = 模型输入块 (94x24) —— 就是模型真正吃到的那张小图, 逐像素一致, 直接当训练素材
static volatile int g_img_mode = 0;
static uint8_t g_crop_u8[IMG_W * IMG_H * 3] __attribute__((aligned(16)));  // 模式 2: 输入张量反量化回来的 RGB (JPEG 源序)。必须 16 字节对齐 —— esp_new_jpeg v0.6 起在 S3 上会检查编码器输入缓冲的对齐
static uint8_t *g_tx_rgb = nullptr;              // RGB888 (16 字节对齐), PSRAM
static uint8_t *g_tx_jpg = nullptr;              // JPEG 输出缓冲, PSRAM
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

/** 模式 2: 把"模型输入块"发出去。图只有 94x24, 但它是训练素材的正品 —— 和推理输入逐像素一致 */
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

// P5.12: 精简日志 —— roi_locate 定位失败时把原因记在这里, 由 recognize_once 合成一行打印
static const char *g_roi_note = "(未知原因)";
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
    if (v < 46) return false;                  // V >= 46
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
    if (deg >= 180 && deg <= 270) return s >= 35;   // 蓝牌 (P5.17: 由 [200,248]+S43 放宽)
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
    if (fill < 0.45) { fit->why = "旋转矩形填充不足 (<45%)"; return false; }   // 散块/被背景撑大 -> 不值得摆正, 让调用方用轴对齐
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

    // ---- 4) 按根节点归并外接框, 再用原脚本的 score 挑最好的 ----
    roi_slot_t slots[ROI_MAX_SLOTS];
    int nslot = 0;
    for (int y = 0; y < ROI_GRID_H; y++) {
        for (int x = 0; x < ROI_GRID_W; x++) {
            const int i = y * ROI_GRID_W + x;
            if (!bin[i]) continue;
            const int root = roi_find_root(parent, i);
            int s = -1;
            for (int k = 0; k < nslot; k++) {
                if (slots[k].root == root) { s = k; break; }
            }
            if (s < 0) {
                if (nslot >= ROI_MAX_SLOTS) continue;
                s = nslot++;
                slots[s].root = root;
                slots[s].x1 = slots[s].x2 = x;
                slots[s].y1 = slots[s].y2 = y;
                slots[s].area = 0;
            }
            if (x < slots[s].x1) slots[s].x1 = x;
            if (x > slots[s].x2) slots[s].x2 = x;
            if (y < slots[s].y1) slots[s].y1 = y;
            if (y > slots[s].y2) slots[s].y2 = y;
            slots[s].area++;
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
    bool c_ok[ROI_MAX_SLOTS], c_okr[ROI_MAX_SLOTS];
    const char *c_why[ROI_MAX_SLOTS];   // P5.22: 每个候选"为什么通过/被淘汰" (详细模式打出来)
    static roi_fit_t c_fit[ROI_MAX_SLOTS];   // 每个候选的旋转拟合结果 (大数组放静态区)
    for (int k = 0; k < ROI_MAX_SLOTS; k++) {
        c_ratio[k] = c_axis_ratio[k] = c_dens[k] = c_denseff[k] = c_score[k] = 0.0f;
        c_ok[k] = c_okr[k] = false;
        c_why[k] = "未评估";
        c_fit[k].ok = false;
    }
    for (int k = 0; k < nslot; k++) {
        if (slots[k].area < min_cells) { c_why[k] = "连通域太小"; continue; }
        const float bw = (float)(slots[k].x2 - slots[k].x1 + 1);
        const float bh = (float)(slots[k].y2 - slots[k].y1 + 1);
        c_axis_ratio[k] = bw / bh;
        roi_fit_rotated(cell, bin, parent, &slots[k], w, h, &c_fit[k]);
        const float ratio = c_fit[k].ok ? c_fit[k].ratio : c_axis_ratio[k];
        c_ratio[k] = ratio;
        if (ratio < ROI_RATIO_LO || ratio > ROI_RATIO_HI) {
            // 倾斜的牌照最常死在这一条: 旋转拟合没成功 -> 退回轴对齐外接框 -> 外接框被倾斜撑胖 -> 比例不过
            c_why[k] = c_fit[k].ok ? "旋转宽高比越界" : "宽高比越界(拟合失败, 退回轴对齐)";
            continue;
        }
        c_okr[k] = true;
        c_dens[k] = (float)slots[k].area / (bw * bh);
        // 有效填充率: 摆正得了就用旋转矩形的填充率 (不随倾角掉), 否则退回轴对齐外接框密度
        c_denseff[k] = c_fit[k].ok ? c_fit[k].fill : c_dens[k];
        c_score[k] = (float)slots[k].area / (1.0f + fabsf(ratio - ROI_RATIO_IDEAL));
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
        ESP_LOGI(TAG, "候选小结: 列出 %d 个 / 连通域共 %d 个; 门槛 = 面积>=%d格 比例%.1f~%.1f 有效填充>=%.0f%%",
                 shown, nslot, min_cells, ROI_RATIO_LO, ROI_RATIO_HI, ROI_MIN_FILL * 100.0f);
    }
    int best_k = -1;
    for (int k = 0; k < nslot; k++) {
        if (!c_ok[k]) continue;
        if (best_k < 0 || c_score[k] > c_score[best_k]) best_k = k;
    }
    // P5.24: 最佳与第二名的得分差距 —— 差距很小说明"这一帧选得勉强", 一旦选错, 后面再准也没用。
    if (verbose && best_k >= 0) {
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
        g_roi_note = "有车牌色, 但没有一块像车牌的长方形 —— 太小 / 太斜 / 被反光洗白";
        if (verbose) {
            ESP_LOGW(TAG, "有车牌色但没找到合格的矩形 (车牌色格数 %d/%d, 其中填充不足淘汰 %d 个) —— 可能太小/倾斜, 或蓝色被反光洗白",
                     color_cells, ROI_GRID_N, dens_rej);
            roi_dump_mask(cell, ROI_CELL_NEED, nullptr);
        }
        return false;
    }

    // ---- 4.5) 贴到画面边缘的框一律拒绝 ----
    // 连通域顶到画面边界 => 车牌有一截在画面外, 裁出来的图必然缺字。
    // 实测 (2026-09-28 连续模式 105 轮): 贴边的 25 轮里只有 1 轮侥幸读对, 其余全是乱码;
    // 同样大小/比例但不贴边的那些轮次大部分能读对。所以宁可"不出结果", 也不要沉默地出乱码。
    {
        const roi_slot_t *bs = &slots[best_k];
        const bool clip_l = (bs->x1 == 0);
        const bool clip_r = ((bs->x2 + 1) * ROI_GRID_STEP >= w);
        const bool clip_t = (bs->y1 == 0);
        const bool clip_b = ((bs->y2 + 1) * ROI_GRID_STEP >= h);
        if (clip_l || clip_r || clip_t || clip_b) {
            char edges[16] = "";
            if (clip_l) strcat(edges, "左");
            if (clip_r) strcat(edges, "右");
            if (clip_t) strcat(edges, "上");
            if (clip_b) strcat(edges, "下");
            static char note[192];   // 中文按字节算, 80 字节不够 (GCC format-truncation 会直接报错)
            snprintf(note, sizeof(note), "车牌贴到画面%s边缘被切掉了 —— 把它完整移进画面, 四周留一成余量", edges);
            g_roi_note = note;
            if (verbose) {
                ESP_LOGW(TAG, "选中的框贴到画面%s边缘 (x[%d,%d) y[%d,%d) 比例 %.2f 有效填充 %.0f%%) -> 车牌被画面切掉, 拒绝出结果",
                         edges, bs->x1 * ROI_GRID_STEP, (bs->x2 + 1) * ROI_GRID_STEP,
                         bs->y1 * ROI_GRID_STEP, (bs->y2 + 1) * ROI_GRID_STEP,
                         c_ratio[best_k], c_denseff[best_k] * 100.0f);
                static int edge_mask_left = 5;   // 掩码很贵, 只在前几次拒绝时打
                if (edge_mask_left > 0) { edge_mask_left--; roi_dump_mask(cell, ROI_CELL_NEED, nullptr); }
            }
            return false;
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
        if (s > 1.0f) {
            b = (int)(mb + (float)(b - (int)mb) * s + ((b > mb) ? 0.5f : -0.5f));
            g = (int)(mg + (float)(g - (int)mg) * s + ((g > mg) ? 0.5f : -0.5f));
            r = (int)(mr + (float)(r - (int)mr) * s + ((r > mr) ? 0.5f : -0.5f));
            if (b < 0) b = 0; else if (b > 255) b = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (r < 0) r = 0; else if (r > 255) r = 255;
        }
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
                                 float v_lo, float v_hi,      // P5.9: 短边方向的取样区间 (收边后)
                                 int8_t *dst, const int8_t *lut,
                                 int *out_nx, int *out_ny, float *out_norm_s) {
    // P5.13: u/v 都往外多取 ROI_CROP_PAD —— quad_eval 是双线性外插, 等于把四边形按同心放大
    const float span_v = v_hi - v_lo;
    const float u_base = -ROI_CROP_PAD;
    const float v_base = v_lo - ROI_CROP_PAD * span_v;
    const float du = (1.0f + 2.0f * ROI_CROP_PAD) / (float)IMG_W;
    const float dv = (span_v * (1.0f + 2.0f * ROI_CROP_PAD)) / (float)IMG_H;
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
            // P5.13: 采样区间外扩了, 每个输出像素覆盖的源面积也等比变大, 不补这一下就欠采样(走样)
            const float scale_pad = 1.0f + 2.0f * ROI_CROP_PAD;
            const float len_x = sqrtf(ex * ex + ey * ey) * scale_pad;   // = 一个输出像素覆盖多少个源像素
            const float len_y = sqrtf(fx * fx + fy * fy) * scale_pad;
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
                           bool verbose) {
    const int fn = ++g_frame_no;   // P5.12: 帧号 —— 之后每行日志都带 #n, 便于对照
    g_crop_q.valid = 0;            // P5.24: 本帧的裁剪块质量由预处理那一趟填; 先清掉, 免得日志报上一帧的数字
    if (verbose) ESP_LOGI(TAG, "------------ 会话: %s (帧 #%d) ------------", trigger, fn);

    camera_discard_frames(CAM_DISCARD_FRAMES, CAM_DISCARD_DELAY_MS);

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "拍照失败");
        return false;
    }

    bool ok = false;
    roi_box_t box = {};        // P5.12: 提到 do 外面 —— 帧尾发预览图时还要用
    bool has_roi = false;
    do {
        if (fb->format != PIXFORMAT_RGB565) {
            ESP_LOGE(TAG, "帧格式不是 RGB565: %d", (int)fb->format);
            break;
        }
        // esp-dl 假设数据是紧凑的 (无行填充), 不成立就必须报错而不是将错就错
        const size_t expect = (size_t)fb->width * fb->height * 2;
        if (fb->len != expect) {
            ESP_LOGE(TAG, "帧长度异常: len=%u 期望=%u (存在行填充?)",
                     (unsigned)fb->len, (unsigned)expect);
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
        // P5.13: 与旋转路径一致, 轴对齐回退也要外扩留白(并夹回画面内)
        const int padx = (int)((float)(box.x2 - box.x1) * ROI_CROP_PAD);
        const int pady = (int)((float)(box.y2 - box.y1) * ROI_CROP_PAD);
        int ax1 = box.x1 - padx, ay1 = box.y1 - pady;
        int ax2 = box.x2 + padx, ay2 = box.y2 + pady;
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
                                 v_lo, v_hi,
                                 (int8_t *)model_input->data, norm_lut, &ss_nx, &ss_ny, &norm_s);
        } else {
            if (verbose) ESP_LOGW(TAG, "旋转拟合不可用 -> 退回轴对齐裁剪 (框宽高比 %.2f)", box.ratio);
            dl::image::resize(src, dst, dl::image::DL_IMAGE_INTERPOLATE_BILINEAR, caps, norm_lut,
                              crop_area, &scale_x, &scale_y);
        }
        int64_t pre_ms = (esp_timer_get_time() - t_pre) / 1000;

        // P5.16: 模式 2 就把"模型真正吃到的那块图"存下来。必须在这里做 —— run() 之后
        // 这块显存会被复用 (见下面那段警告), 那时候读到的已经是别的张量了。
        if (g_img_mode == 2) {
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
                roi_probe_profile(fb->buf, (int)fb->width, (int)fb->height, box.qx, box.qy, v_lo, v_hi);
            }
        }


        int64_t t_inf = esp_timer_get_time();
        model->run();
        int64_t inf_ms = (esp_timer_get_time() - t_inf) / 1000;

        output_float->assign(model->get_outputs().begin()->second);
        decode_report_t rep;   // P5.24: 顺带算出每一步的置信度 (不改变解码结果)
        std::string plate = greedy_decode_impl((const float *)output_float->data, &rep);

        // P5.13: 蓝牌永远是 7 位(省 + 字母 + 5 位)。外扩 8% 之后, 偶尔会从画面外沿的暗边上
        //   再"读"出一个字母挂在尾巴上 (实测 京Q06666 -> 京Q06666L / 京Q06666U, 21 帧里 12 帧)。
        //   蓝底时(裁剪块 B > G)把多出来的那个尾字符削掉; 绿牌是 8 位, 不动。
        //   10 字节 = 1 个汉字(3) + 7 个 ASCII, 正好是"多了一位"的情形。
        if (plate.size() == 10 && cm_b > cm_g) {
            if (verbose) ESP_LOGW(TAG, "语法修正: %s -> 去掉尾巴上多出的一位 (蓝牌应为 7 位)", plate.c_str());
            plate.pop_back();
        }

        // P5.5: 把"这块牌实际被多少个源像素平均出来"打出来 —— 采样数 <1 说明牌太小(信息本就不够),
        // 而不是模型不行; 这个数字和 RESULT 一起看, 才能分清"欠采样"和"认字不行"。
        // P5.12: 连续模式把一帧压成"一行指标 + 一行结果"; 其余诊断只在按 BOOT 时打
        if (ss_nx > 0) {
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 收边[%.0f%%,%.0f%%] | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",
                     fn, box.x2 - box.x1, box.y2 - box.y1, box.area_pct,
                     box.rot_ok ? "" : " 轴对齐",
                     v_lo * 100.0f, v_hi * 100.0f,
                     (long long)roi_ms, (long long)pre_ms, (long long)inf_ms,
                     ss_nx, ss_ny, norm_s, cm_b, cm_g, cm_r);

        } else {
            ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s | 定位%lld 预处理%lld 推理%lld ms",
                     fn, box.x2 - box.x1, box.y2 - box.y1, box.area_pct,
                     box.rot_ok ? "" : " 轴对齐",
                     (long long)roi_ms, (long long)pre_ms, (long long)inf_ms);
        }
        // P5.24: 结果后面直接跟置信度 —— 连续模式(每帧 2 行)也能一眼看出"这次是模型有把握还是瞎猜"
        if (plate.empty()) {
            ESP_LOGW(TAG, "#%d >>> RESULT: (未识别到车牌) | 置信 %.0f%%",
                     fn, rep.mean_top1 * 100.0f);
        } else {
            ESP_LOGI(TAG, "#%d >>> RESULT: %s | 置信 %.0f%% (最弱步 %.0f%%)", fn, plate.c_str(),
                     rep.mean_top1 * 100.0f, rep.min_top1 * 100.0f);
        }

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
                ESP_LOGI(TAG, "裁剪块质量: 亮度 p5=%d p95=%d 跨度=%d | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 对比度归一 x%.2f",
                         g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.spread, g_crop_q.over_pct,
                         g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                         g_crop_q.norm_s);
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
            //   已经打过完全相同的一份 (同样的框、同样的收边区间), 纯属重复。
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
                        ESP_LOGI(TAG, "#%d 质量: 亮度跨度 %d (p5=%d p95=%d) | 过曝 %d%% 死黑 %d%% | 锐度 %.1f | 均值 B%d G%d R%d | 对比度归一 x%.2f",
                                 fn, g_crop_q.spread, g_crop_q.p_lo, g_crop_q.p_hi, g_crop_q.over_pct,
                                 g_crop_q.dark_pct, g_crop_q.sharp, g_crop_q.mb, g_crop_q.mg, g_crop_q.mr,
                                 g_crop_q.norm_s);
                    } else {
                        ESP_LOGI(TAG, "#%d 质量: n/a (轴对齐回退, 这一帧没走光度归一化)", fn);
                    }
                }
            }
        }
#endif
        ok = true;
    } while (0);

    // P5.12: 不管有没有定位到车牌, 都把这一帧(缩一半)编码成 JPEG 发给串口助手预览。
    // 没定位到也发 —— 正好让你看见"相机到底看见了什么", 比看文字直观。
    // P5.14: 绿框只在详细模式(开机/按 BOOT)那一帧画 —— 那帧是专门用来看"框套得准不准"的;
    //        定时连续帧不画框, 因为那些才是要采下来当训练素材的图 (画上去还得再擦一遍, 擦不干净)。
    if (g_img_mode == 2) {
        // 模式 2 发的是"喂给模型的那张小图": 这一帧没跑到推理(没定位到车牌)就没有东西可发。
        // 说一句, 免得以为图片输出坏了 —— 限制 5 s 一次, 不刷屏。
        if (ok) {
            img_tx_send_crop();
        } else {
            static int64_t last_hint_us = 0;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_hint_us > 5000000) {
                last_hint_us = now_us;
                ESP_LOGW(TAG, "模式 2: 这一帧没定位到车牌, 没有 94x24 输入块可发 (只发有牌的那几帧)");
            }
        }
    } else {
        img_tx_send(fb->buf, (int)fb->width, (int)fb->height,
                   (has_roi && g_img_mode == 1) ? &box : nullptr);
    }

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

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== LPRNet on ESP32-S3 —— 固件 P5.28 ===");
    ESP_LOGI(TAG, "    构建时间: %s %s —— 开机看到 P5.28 才说明烧进去的是新固件", __DATE__, __TIME__);
    ESP_LOGI(TAG, "    连续模式: 每帧 2 行(#n 指标行 + #n RESULT, 结果带置信度); 结果可疑时自动补打一行诊断");
    ESP_LOGI(TAG, "    完整诊断(掩码+候选表+覆盖率剖面): 长按 BOOT >=2s 走一轮; 自动诊断开关 = main.cpp 的 AUTO_DIAG_ON_SUSPECT");
    ESP_LOGI(TAG, "    串口图片: $IMG,<len> + JPEG + CRC32(大端4B) + $END  -> BY串口助手选「二进制帧」, 波特率 %d", CONFIG_ESP_CONSOLE_UART_BAUDRATE);
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
        if (AUTO_PERIOD_MS > 0) {
            ESP_LOGI(TAG, "触发方式: 开机 1 次 + 每 %u ms 自动 1 次 + 短按 BOOT 立即拍一帧",
                     (unsigned)AUTO_PERIOD_MS);
            ESP_LOGI(TAG, "说明: 周期 %u ms => 背靠背连续识别, 精简模式(每帧 2 行); 短按(<1s)BOOT = 循环切换图片输出(当前 %d), 长按(>=2s)BOOT = 详细模式(掩码+候选表)",
                     (unsigned)AUTO_PERIOD_MS, g_img_mode);
        } else {
            ESP_LOGI(TAG, "触发方式: 开机 1 次 + 短按(<1s)BOOT 切图片模式并拍一帧 + 长按(>=2s)BOOT 出详细模式");
            ESP_LOGI(TAG, "图片输出模式: 0=干净预览 1=预览+绿框 2=模型输入块94x24(训练素材)");
        }
    } else {
        ESP_LOGE(TAG, "摄像头不可用, 先跑一次内嵌样张自检; 之后每 5s 重试摄像头");
        run_embedded_selfcheck(model, model_input, output_float);
    }

    // ---- 5. 主循环: 按钮 / 定时触发识别 ----
    TickType_t next_auto = 0;          // 0 => 立刻跑第一次
    TickType_t next_cam_retry = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();

        if (camera_ok) {
            // P5.15: 短按(<1s) = 循环切换图片输出模式; 长按(>=2s) = 详细模式识别一次。
            //        中间 1~2s 留空档: 想按短按但手抖按久了、想按长按但没按够, 都当作没按, 免得误触。
            // 以中断锁存为主; 万一 ISR 装不上, 电平轮询这条老路仍然兜底 (那条路量不出时长, 一律当短按)。
            const bool btn_down = (boot_btn_latched || gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0);
            if (btn_down) {
                boot_btn_latched = false;
                const int32_t press_ms = boot_press_ms;
                const int32_t end_ms = (boot_release_ms != 0) ? boot_release_ms
                                       : (int32_t)(esp_timer_get_time() / 1000);
                const int32_t dur_ms = end_ms - press_ms;
                const bool long_press = (press_ms != 0) && (dur_ms >= 2000);
                const bool short_press = (press_ms == 0) || (dur_ms < 1000);   // press_ms==0 = 轮询兜底, 量不出时长, 一律当短按
                if (long_press) {
                    img_tx_reset("BOOT 长按");   // P5.20: 顺手复位图片输出 —— 卡住时按一下就能救回来
                    recognize_once(model, model_input, output_float, norm_lut, "BOOT 长按(详细)", true);
                } else if (short_press) {
                    img_tx_reset("BOOT 短按");   // P5.20: 同理, 不用再重启单片机
                    g_img_mode = (g_img_mode + 1) % 3;
                    static const char *mode_name[3] = {
                        "干净预览图 (320x240, 画面/采数据)",
                        "预览图 + 绿框 (看框套得准不准)",
                        "模型输入块 (94x24, 训练素材 —— 和推理输入逐像素一致)"};
                    ESP_LOGW(TAG, "图片输出: %s", mode_name[g_img_mode]);
                    recognize_once(model, model_input, output_float, norm_lut, "BOOT 短按(切图片模式)", false);
                } else {
                    ESP_LOGW(TAG, "按键 %d ms 落在 1~2s 空档里, 当作没按 (短按<1s 切图片模式, 长按>=2s 详细模式)", (int)dur_ms);
                }
                boot_press_ms = 0;
                boot_release_ms = 0;
                while (gpio_get_level((gpio_num_t)BOOT_BTN_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));   // 等松手, 免得一次长按连跑好几轮
                }
                boot_btn_latched = false;   // 松手过程中的抖动边沿一并清掉
                next_auto = xTaskGetTickCount() + pdMS_TO_TICKS(AUTO_PERIOD_MS);
            } else if (AUTO_PERIOD_MS > 0 && now >= next_auto) {
                const bool first = (next_auto == 0);   // 开机头一次用详细模式, 之后走精简
                recognize_once(model, model_input, output_float, norm_lut,
                               first ? "开机" : "定时(连续)", first);
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
