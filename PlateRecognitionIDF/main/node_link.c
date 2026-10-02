/**
 * node_link.c —— 摄像头 -> 节点单片机 链路 (P5.69)
 *
 * 接口与协议说明写在 node_link.h 里。这里只提醒三处坑:
 *   1) 默认走控制台口, 所有输出一律以 "\n" 结尾 —— 控制台 VFS 会把 LF 转成 CRLF,
 *      自己再写 "\r\n" 会变成 "\r\r\n"。想原样发字节(像 $IMG 那样)得绕开 VFS, 那是另一条路。
 *   2) 本文件是纯 C, 解析/打包逻辑以后能整段抄到 STM32 节点侧, 两边语法保持一致。
 *   3) P5.69 起"节点口"和"电脑口"是两路独立输入(各自一个行缓冲, 绝不串行解析);
 *      输出按目的路由: $PLATE 和"回给节点的回复"走节点口, 日志和"给电脑的回复"走电脑口。
 *      P5.77: 每帧自动上行 $PLATE 只在真节点口出 (时期 A 干脆不打); 电脑口只看结果块/未识别块。
 */
#include "node_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#if NODE_LINK_USE_UART1
#include "driver/uart.h"
#define NL_UART   UART_NUM_1
#endif

static const char *TAG = "nodelink";

#define NL_LINE_MAX   96
#define NL_RX_CHUNK   64
#define NL_POLL_ROUND 8      /* 一路一轮 poll 最多吃 8*64 = 512 字节, 不霸占主循环 */

/* 这条命令/数据是从哪条口来的 */
typedef enum {
    NL_SRC_NODE = 0,   /* 节点口: UART1 (时期 B) / 控制台(时期 A, 电脑串口助手当"假节点") */
    NL_SRC_PC   = 1,   /* 电脑口: 控制台 stdin (时期 A 它是唯一入口; 时期 B 它是调试口) */
    NL_SRC_COUNT
} nl_src_t;

/* ==================== 状态 ==================== */

typedef struct {
    char   line[NL_LINE_MAX];
    size_t len;
} nl_rx_t;

static nl_rx_t s_rx[NL_SRC_COUNT];      /* 每条口一个行缓冲, 互不干扰 */

static char s_last_plate[24] = "-";
static bool s_last_valid;
static int  s_last_conf;
static int  s_last_frame;

static uint32_t s_up_plate, s_rx_lines, s_bad_cmds, s_denied;
static const char *s_fw = "?";
static node_link_extra_cmd_cb_t s_extra_cb;

static volatile bool s_ctrl_pc;        /* false = 正常模式(节点是主人, 上电默认) */
static volatile bool s_ctrl_event;     /* 控制权刚切过, 等主固件消费 */

/* P5.74: 现在生效的图片档位(0=没在发图)。非 0 时每条 $PLATE 尾巴上附加 ",img=<档位>" 当提醒,
 *   免得"图还开着"只能靠自己记得 (主固件在 img_mode_apply() 里同步过来) */
static volatile int s_img_hint;

/* ==================== 镜像 ==================== */

/*
 * 镜像 —— 把"节点口 <-> 摄像头"的对话抄一份到电脑口(控制台)。
 *   为什么要有: 真节点接上以后, 节点那条线是给节点自己看的, 电脑上什么都看不见;
 *   调试时想知道"摄像头到底回没回、回了什么", 只能靠这份抄本 (节点侧的范式见 bsp_lora.c:
 *   发真帧 + 打一行人话摘要)。
 *   P5.75: 去掉 AT+MIRROR 开关 —— 没有"要关掉"的场景, 一直开着。时期 A(只有一条 USB 线)
 *   两条口本来就是同一根线, 抄了就是同一句话打两遍, 所以那时自动不抄; 时期 B 默认抄。
 */
static bool nl_mirror_on(void)
{
#if NODE_LINK_USE_UART1
    return true;          /* 时期 B: 抄 */
#else
    return false;         /* 时期 A: 同一条线, 抄了就重复 */
#endif
}

static void nl_mirror(const char *dir, const char *line, size_t n)
{
    if (!nl_mirror_on()) return;

    char body[192];
    size_t m = (n < sizeof(body) - 1) ? n : sizeof(body) - 1;
    memcpy(body, line, m);
    while (m > 0 && (body[m - 1] == '\n' || body[m - 1] == '\r')) m--;
    body[m] = '\0';
    if (m == 0) return;

    /* $PLATE 那行再附一句人话 —— 人和节点侧代码都一眼能看懂 */
    if (strncmp(body, "$PLATE,", 7) == 0) {
        char tmp[sizeof(body)];
        snprintf(tmp, sizeof(tmp), "%s", body);
        char *c1 = strchr(tmp + 7, ',');
        if (c1) {
            *c1 = '\0';
            char *c2 = strchr(c1 + 1, ',');
            if (c2) {
                *c2 = '\0';
                const char *plate = tmp + 7;
                ESP_LOGI(TAG, "[镜像] %s: %s   (车牌=%s, 置信=%s%%, 第 %s 帧)", dir, body,
                         (strcmp(plate, "-") == 0) ? "没认出来" : plate, c1 + 1, c2 + 1);
                return;
            }
        }
    }
    ESP_LOGI(TAG, "[镜像] %s: %s", dir, body);
}

/* ==================== 收 / 发原语 ==================== */

/* 按目的口写字节。时期 A 只有一条口(控制台), 两个目的都落到 stdout */
static void nl_write_to(nl_src_t dst, const char *s, size_t n)
{
#if NODE_LINK_USE_UART1
    if (dst == NL_SRC_NODE) {
        nl_mirror("发给节点", s, n);          /* P5.70: 真发出去的同时, 电脑口留一份 */
        uart_write_bytes(NL_UART, s, n);
        return;
    }
#else
    if (dst == NL_SRC_NODE) nl_mirror("发给节点", s, n);
#endif
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

/* 回复一行(自动补 '\n')给指定的那条口 —— 千万别自己再写 '\r'
 *
 * P5.77: console_ok=false 是"只给机器看、不给人看"的那些行 (每帧自动上行 $PLATE):
 *   时期 A(节点口==电脑口) 干脆一个字都不打 —— 人看的东西由主固件的结果块/未识别块负责,
 *     $PLATE 是给节点解析的, 两条口径分开才不重复;
 *   时期 B 照发 UART1, 只是不往电脑口镜像 (镜像那条留给"人主动要看线上格式"的场合)。
 */
static void nl_vout_to(nl_src_t dst, bool console_ok, const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(buf) - 2) n = (int)(sizeof(buf) - 2);
    buf[n++] = '\n';

    if (!console_ok && dst == NL_SRC_NODE) {
#if NODE_LINK_USE_UART1
        uart_write_bytes(NL_UART, buf, (size_t)n);
#endif
        return;
    }
    nl_write_to(dst, buf, (size_t)n);
}

static void nl_out_to(nl_src_t dst, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nl_vout_to(dst, true, fmt, ap);
    va_end(ap);
}

/* 只走节点口、不往电脑口抄的那一类 (每帧自动上行) */
static void nl_out_to_quiet(nl_src_t dst, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nl_vout_to(dst, false, fmt, ap);
    va_end(ap);
}

static int nl_read_stdin(uint8_t *buf, size_t cap)
{
    const ssize_t n = read(STDIN_FILENO, buf, cap);
    if (n < 0) return 0;              /* EAGAIN: 现在没数据 */
    return (int)n;
}

/* 大小写不敏感的比较 (参数值 PC / NODE 用) */
static bool nl_arg_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ==================== 控制权 ==================== */

bool node_link_ctrl_is_pc(void) { return s_ctrl_pc; }

const char *node_link_ctrl_str(void) { return s_ctrl_pc ? "PC" : "NODE"; }

bool node_link_take_ctrl_event(void)
{
    if (!s_ctrl_event) return false;
    s_ctrl_event = false;
    return true;
}

static bool nl_is_owner(nl_src_t src)
{
    return s_ctrl_pc ? (src == NL_SRC_PC) : (src == NL_SRC_NODE);
}

/*
 * 旁观者来命令 -> 只回一行"没有权限" (P5.74: 直接拒绝, 不解释它能发什么、也不提怎么抢)。
 * 若是"调试模式下的节点口"被拒, 再往电脑口镜像一行 —— 调试时能看见节点在敲什么,
 * 否则只看到行为异常却不知道为什么 (时期 B 的节点/电脑是两条线, 这一行才真的分得开)。
 */
static void nl_deny(nl_src_t src, const char *verb)
{
    s_denied++;
    nl_out_to(src, "ERR:没有权限: 现在是%s模式, 控制权在%s手里",
              s_ctrl_pc ? "调试" : "正常", s_ctrl_pc ? "电脑" : "节点");
    if (src == NL_SRC_NODE) {
        ESP_LOGW(TAG, "[镜像] 节点口刚发的 %s 被拒了: 现在是调试模式, 控制权在电脑手里",
                 (verb && *verb) ? verb : "AT");
    }
}

/* ==================== 上行 ==================== */

static void nl_set_last(const char *plate, bool valid, int conf, int frame)
{
    snprintf(s_last_plate, sizeof(s_last_plate), "%s", (plate && *plate) ? plate : "-");
    s_last_valid = valid;
    s_last_conf  = conf;
    s_last_frame = frame;
}

/* 上行只走节点口 —— 这几行就是给节点看的。
 * 不合法/没认出来一律发 "-": 节点只看这个字段判断"有没有车牌", 别把"疑似误检"的碎字喂过去
 * (否则节点会把 皖/京8 这种半截结果当成真车牌上报)。
 * P5.74: 每一帧就这一行 —— 它同时是 AT+RUN 的回复(不再另外回 +RUN:), 也不再有第二个结果行。
 *   发图时尾巴多一个 ",img=<档位>"; 节点按逗号切字段、只读前 3 个即可, 多出来的那个可以不管。
 * P5.77: on_console=false = 每帧自动上行, 电脑口不打 (人看结果块/未识别块, 免得同一帧打两遍);
 *   on_console=true = 人主动要看线上格式 (AT+TEST / AT+PUSH), 电脑口也照打一份。 */
static void nl_send_plate(bool valid, const char *plate, int conf, int frame, bool on_console)
{
    const char *p = (valid && plate && *plate) ? plate : "-";
    const int   c = valid ? conf : 0;
    if (s_img_hint > 0) {
        if (on_console) nl_out_to(NL_SRC_NODE, "$PLATE,%s,%d,%d,img=%d", p, c, frame, (int)s_img_hint);
        else            nl_out_to_quiet(NL_SRC_NODE, "$PLATE,%s,%d,%d,img=%d", p, c, frame, (int)s_img_hint);
    } else {
        if (on_console) nl_out_to(NL_SRC_NODE, "$PLATE,%s,%d,%d", p, c, frame);
        else            nl_out_to_quiet(NL_SRC_NODE, "$PLATE,%s,%d,%d", p, c, frame);
    }
    s_up_plate++;
}

/* ==================== 下行: AT 命令 ==================== */

static void nl_help(nl_src_t src)
{
    nl_out_to(src, "-----");
    nl_out_to(src, "   AT                  探活 -> OK");
    nl_out_to(src, "   AT+HELP             本命令表");
    nl_out_to(src, "   AT+CTRL=NODE|PC     控制权: NODE=正常模式(节点是主人, 上电默认), PC=调试模式(电脑是主人)");
    nl_out_to(src, "   AT+INFO             一眼看全: 版本/运行时长/统计(+INFO) + 每条命令当前值(+CFG)");
    nl_out_to(src, "   AT+PLATE            查最近一次识别结果");
    nl_out_to(src, "   AT+TEST=<车牌>      注入一个测试车牌, 立刻走一遍上行");
    nl_out_to(src, "   AT+PUSH             把最近一次结果重推一遍");
    nl_out_to(src, "   AT+RUN              立刻拍一帧 (电脑口回结果块/未识别原因; 节点口回 $PLATE; 约 1 秒)");
    nl_out_to(src, "   AT+TRIG=<0|1>       1=只听 AT+RUN(默认), 0=连续自动识别");
    nl_out_to(src, "   AT+LOG=<0|1>         日志档位: 1=详细, 0=简单(默认)");
    nl_out_to(src, "   AT+IMG=<0..3>       串口图片输出档位");
    nl_out_to(src, "   AT+PAD=<0..3>       取样几何档");
    nl_out_to(src, "-----");
    nl_out_to(src, "   带 \"=\" 是设置(成功回 OK, 失败回 ERR:<原因>); 不带 \"=\" 是查询(只回 +XXX:<当前值>)");
    nl_out_to(src, "   每条命令后面加 \"?\" 看它的取值含义 (例如 AT+IMG?)");
    nl_out_to(src, "   AT+HELP / AT+INFO / AT+XXX? 只读, 不看控制权, 谁都能发");
}

/*
 * P5.74: AT+XXX? —— 看这条命令每个取值的含义。
 *   只查文档: 不碰权限闸门、不改任何状态, 旁观者(正常模式的电脑口 / 调试模式的节点口)也能看。
 *   表写在这里而不是外挂分支: 外挂回调只回一行(128 字节), 装不下多行说明。
 */
static void nl_help_cmd(nl_src_t src, const char *verb)
{
    if (strcmp(verb, "CTRL") == 0) {
        nl_out_to(src, "+CTRL?:控制权归谁 —— 谁的主人能发全部命令, 另一个只能发 AT+CTRL");
        nl_out_to(src, "   NODE = 正常模式: 节点是主人 (上电默认)");
        nl_out_to(src, "   PC   = 调试模式: 电脑是主人");
        nl_out_to(src, "   设置 AT+CTRL=NODE|PC 回 OK;  查询 AT+CTRL 回 +CTRL:NODE|PC");
    } else if (strcmp(verb, "INFO") == 0) {
        nl_out_to(src, "+INFO?:一眼看全 —— 没有取值, 直接发 AT+INFO");
        nl_out_to(src, "   第1行 +INFO:<版本>,运行<秒>s,上线<行>,收行<行>,坏命令<次>,被拒<次>,控制权=<NODE|PC>,镜像=<开|关>,链路=<console|uart1>");
        nl_out_to(src, "   第2行 +CFG:LOG=<0|1>,IMG=<0..3>,TRIG=<0|1>,PAD=<0..3>,PLATE=<车牌>,<置信度>,<帧号> —— 每条带取值的命令的当前值");
    } else if (strcmp(verb, "PLATE") == 0) {
        nl_out_to(src, "+PLATE?:查最近一次识别结果 —— 没有取值, 直接发 AT+PLATE");
        nl_out_to(src, "   回 +PLATE:<车牌>,<置信度>,<帧号>,<是否合法>; 没认出来时车牌字段是 - 、是否合法=0");
    } else if (strcmp(verb, "TEST") == 0) {
        nl_out_to(src, "+TEST?:注入一个测试车牌, 立刻走一遍上行(不碰摄像头)");
        nl_out_to(src, "   用法 AT+TEST=<车牌>, 例如 AT+TEST=豫F·SQ818; 没有可查询的值");
    } else if (strcmp(verb, "PUSH") == 0) {
        nl_out_to(src, "+PUSH?:把最近一次结果重推一遍 —— 没有取值, 直接发 AT+PUSH");
    } else if (strcmp(verb, "RUN") == 0) {
        nl_out_to(src, "+RUN?:立刻拍一帧 (节点触发拍照就用这条)");
        nl_out_to(src, "   不回 OK: 约 1 秒后直接出结果 —— 电脑口是结果块或\"未识别\"块(含原因); 节点口是 $PLATE,<车牌>,<置信度>,<帧号>");
        nl_out_to(src, "   还在往串口发图时, 那一行的尾巴上会多一个 img=<档位> —— 提醒你图忘了关");
    } else if (strcmp(verb, "TRIG") == 0) {
        nl_out_to(src, "+TRIG?:触发方式");
        nl_out_to(src, "   1 = 只听 AT+RUN, 平时不拍 (默认)");
        nl_out_to(src, "   0 = 连续自动识别");
        nl_out_to(src, "   设置 AT+TRIG=0|1 回 OK;  查询 AT+TRIG 回 +TRIG:0|1");
    } else if (strcmp(verb, "LOG") == 0) {
        nl_out_to(src, "+LOG?:日志档位");
        nl_out_to(src, "   0 = 简单日志 (默认): 只打结果块 —— 没有 ROI 行 / 候选表 / 掩码 / 警告");
        nl_out_to(src, "   1 = 详细日志: 每帧补打完整诊断 (定位 / 候选表 / 掩码 / 缩略图 / 各种警告)");
        nl_out_to(src, "   设置 AT+LOG=0|1 回 OK;  查询 AT+LOG 回 +LOG:0|1");
    } else if (strcmp(verb, "IMG") == 0) {
        nl_out_to(src, "+IMG?:串口图片输出档位");
        nl_out_to(src, "   0 = 关 (不发图; 默认档, 接真节点用这档)");
        nl_out_to(src, "   1 = 干净预览 320x240");
        nl_out_to(src, "   2 = 预览 + 绿框");
        nl_out_to(src, "   3 = 模型输入块 94x24");
        nl_out_to(src, "   权限: 只有电脑口能改这个档(节点口发了没用); 正常/调试模式都一样能改");
        nl_out_to(src, "   设置 AT+IMG=0..3 回 OK;  查询 AT+IMG 回 +IMG:<档位>");
    } else if (strcmp(verb, "PAD") == 0) {
        nl_out_to(src, "+PAD?:取样几何档(长边左右留白)");
        nl_out_to(src, "   0 = 左净0% / 右净4%");
        nl_out_to(src, "   1 = 左+2% / 右4%");
        nl_out_to(src, "   2 = 左0% / 右+7% (默认)");
        nl_out_to(src, "   3 = 左-4% / 右+7%");
        nl_out_to(src, "   设置 AT+PAD=0..3 回 OK;  查询 AT+PAD 回 +PAD:<档位>");
    } else if (strcmp(verb, "HELP") == 0) {
        nl_out_to(src, "+HELP?:命令表没有取值 —— 直接发 AT+HELP");
    } else if (*verb == '\0') {
        nl_out_to(src, "ERR:未知命令 (发 AT+HELP 看命令表)");
    } else {
        nl_out_to(src, "ERR:未知命令 %s (发 AT+HELP 看命令表)", verb);
    }
}

static void nl_handle_at(nl_src_t src, char *rest)
{
    if (*rest == '\0') {                                     /* 裸 AT = 探活 */
        if (!nl_is_owner(src)) { nl_deny(src, "AT"); return; }
        nl_out_to(src, "OK");
        return;
    }
    if (*rest != '+') {
        s_bad_cmds++;
        nl_out_to(src, "ERR:命令要以 AT+ 开头 (发 AT+HELP 看命令表)");
        return;
    }

    char *verb = rest + 1;
    char *arg  = strchr(verb, '=');
    if (arg) { *arg = '\0'; arg++; }
    for (char *q = verb; *q; q++) {
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    }

    /* P5.74: 命令名后面光跟一个 ? = "看这条命令每个取值的含义"(例如 AT+IMG?);
     *   只查文档 —— 不碰权限闸门、不改任何状态, 旁观者也能看。 */
    const size_t vlen = strlen(verb);
    const bool want_help = (!arg && vlen > 0 && verb[vlen - 1] == '?');
    if (want_help) { verb[vlen - 1] = '\0'; nl_help_cmd(src, verb); return; }

    if (*verb == '\0') {                                     /* "AT+" 这种半截命令 */
        s_bad_cmds++;
        nl_out_to(src, "ERR:未知命令 (发 AT+HELP 看命令表)");
        return;
    }

    /* ---- 权限闸门 ----
     *   放行"不碰控制权也能用/能看"的几条:
     *     AT+CTRL                      两种模式都放行 —— 否则旁观者没法把控制权抢回来
     *     AT+IMG                       只有电脑口能发 (图片只走电脑那条口, 节点要它没用);
     *                                  于是**正常模式下电脑也能开图看画面**, 不必先抢控制权
     *     AT+HELP / AT+INFO / AT+XXX?  只读命令表和状态, 不看控制权, 谁都能看 (P5.79)
     *   其余命令只有"主人"能发。 */
    const bool open_cmd = (strcmp(verb, "CTRL") == 0) ||
                          (strcmp(verb, "HELP") == 0) ||
                          (strcmp(verb, "INFO") == 0) ||
                          (src == NL_SRC_PC && strcmp(verb, "IMG") == 0);
    if (!open_cmd && !nl_is_owner(src)) {
        nl_deny(src, verb);
        return;
    }

    if (strcmp(verb, "CTRL") == 0) {
        if (arg && *arg) {
            bool want_pc;
            if      (nl_arg_eq(arg, "PC"))   want_pc = true;
            else if (nl_arg_eq(arg, "NODE")) want_pc = false;
            else {
                s_bad_cmds++;
                nl_out_to(src, "ERR:用法 AT+CTRL=NODE|PC (发 AT+CTRL? 看取值含义)");
                return;
            }
            if (want_pc != s_ctrl_pc) {
                s_ctrl_pc = want_pc;
                s_ctrl_event = true;                 /* 主固件据此消费这个事件(顺手同步图片档位; P5.74 起档位不再跟控制权挂钩) */
                memset(s_rx, 0, sizeof(s_rx));       /* 丢掉两条口里可能残留的半截命令 */
                /* P5.73: 这里不打日志 —— 紧跟的 OK 就是全部回复 (要状态发 AT+CTRL 查询) */
            }
            nl_out_to(src, "OK");                    /* 设置类命令回 OK; 查询才回值 */
            return;
        }
        nl_out_to(src, "+CTRL:%s", node_link_ctrl_str());
        return;
    }

    if (strcmp(verb, "HELP") == 0) { nl_help(src); return; }

    if (strcmp(verb, "INFO") == 0) {
#if NODE_LINK_USE_UART1
        const char *link = "uart1";
#else
        const char *link = "console";
#endif
        nl_out_to(src, "+INFO:%s,运行%llds,上线%u行,收行%u,坏命令%u,被拒%u,控制权=%s,镜像=%s,链路=%s",
                  s_fw, (long long)(esp_timer_get_time() / 1000000),
                  (unsigned)s_up_plate, (unsigned)s_rx_lines, (unsigned)s_bad_cmds,
                  (unsigned)s_denied, node_link_ctrl_str(), nl_mirror_on() ? "开" : "关", link);
        /* P5.78: 再把"每条带取值的命令"的当前值列一遍 —— 不用逐条 AT+XXX 去查。
         *   LOG/IMG/TRIG/PAD 只有主固件知道, 由外挂分支补齐; PLATE 在链路层, 这里自己接上。 */
        char cfg[128] = "";
        if (s_extra_cb) s_extra_cb("INFO", NULL, cfg, sizeof(cfg));
        if (cfg[0] == '-') cfg[0] = '\0';
        nl_out_to(src, "+CFG:%s%sPLATE=%s,%d,%d",
                  cfg, cfg[0] ? "," : "", s_last_plate, s_last_conf, s_last_frame);
        return;
    }
    if (strcmp(verb, "PLATE") == 0) {
        nl_out_to(src, "+PLATE:%s,%d,%d,%d", s_last_plate, s_last_conf, s_last_frame, s_last_valid ? 1 : 0);
        return;
    }
    if (strcmp(verb, "TEST") == 0) {
        if (!arg || !*arg) { s_bad_cmds++; nl_out_to(src, "ERR:用法 AT+TEST=<车牌>"); return; }
        nl_set_last(arg, true, 100, s_last_frame + 1);
        nl_send_plate(true, s_last_plate, s_last_conf, s_last_frame, true);   /* 先出上行, 再回 OK (人主动要的, 电脑口也打) */
        nl_out_to(src, "OK");
        return;
    }
    if (strcmp(verb, "PUSH") == 0) {
        nl_send_plate(s_last_valid, s_last_plate, s_last_conf, s_last_frame, true);
        nl_out_to(src, "OK");
        return;
    }

    /* P5.74: AT+RUN 既不回 OK、也不另发一行 —— 它的回复就是那一帧的 $PLATE 行(结果 + 可选的 img= 提醒)。
     *   外挂分支这时只做"受理"(把 g_run_request 立起来), 它回的那个 OK 被这里吞掉。 */
    if (strcmp(verb, "RUN") == 0) {
        char ack[32] = "";
        if (s_extra_cb) s_extra_cb(verb, arg, ack, sizeof(ack));
        if (ack[0] == '\0' || ack[0] == '-') {
            s_bad_cmds++;
            nl_out_to(src, "ERR:现在拍不了 (主固件没受理 AT+RUN)");
        }
        return;
    }

    /* 主固件自己的命令 (触发方式 / 详细模式 / 图片模式 ...) 走外挂分支 */
    char resp[128] = "";
    if (s_extra_cb) s_extra_cb(verb, arg, resp, sizeof(resp));
    if (resp[0] && resp[0] != '-') { nl_out_to(src, "%s", resp); return; }

    s_bad_cmds++;
    nl_out_to(src, "ERR:未知命令 %s (发 AT+HELP 看命令表)", verb);
}

static void nl_handle_line(nl_src_t src)
{
    nl_rx_t *rx = &s_rx[src];
    rx->line[rx->len] = '\0';
    rx->len = 0;

    char *p = rx->line;
    while (*p == ' ' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    if (*p == '\0') return;                                  /* 空行: 忽略 */

    s_rx_lines++;
    if (src == NL_SRC_NODE) nl_mirror("节点发来", p, strlen(p));   /* P5.70 */
    if (strncmp(p, "AT", 2) != 0) { s_bad_cmds++; return; }  /* 不是 AT 命令: 静默丢掉 */
    nl_handle_at(src, p + 2);
}

/* 把一条口的字节流喂进它自己的行缓冲 */
static void nl_feed(nl_src_t src, const uint8_t *buf, int n)
{
    nl_rx_t *rx = &s_rx[src];
    for (int i = 0; i < n; i++) {
        const char c = (char)buf[i];
        if (c == '\r') continue;
        if (c == '\n') { nl_handle_line(src); continue; }
        if (rx->len < NL_LINE_MAX - 1) rx->line[rx->len++] = c;
        else rx->len = 0;      /* 这行太长: 整行丢弃, 免得半截命令被当真命令执行 */
    }
}

/* ==================== 对外接口 ==================== */

void node_link_init(const char *fw_version)
{
    if (fw_version && *fw_version) s_fw = fw_version;

    /* 电脑口永远要收得到: 时期 A 它是唯一入口, 时期 B 它是调试口 */
    const int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

#if NODE_LINK_USE_UART1
    const uart_config_t cfg = {
        .baud_rate  = NODE_LINK_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(NL_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(NL_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(NL_UART, NODE_LINK_UART_TX_GPIO, NODE_LINK_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGW(TAG, "节点链路 = UART1 (TX=GPIO%d RX=GPIO%d @%d) —— 接节点 USART3",
             NODE_LINK_UART_TX_GPIO, NODE_LINK_UART_RX_GPIO, NODE_LINK_UART_BAUD);
#else
    ESP_LOGW(TAG, "节点链路 = 控制台口 (电脑串口助手当'假节点': 发 AT, 收结果块/未识别原因)");
#endif
    ESP_LOGW(TAG, "控制权 = 正常模式(NODE, 上电默认): 节点口能发全部命令, 电脑口只能发 AT+CTRL");
    ESP_LOGW(TAG, "    上行 $PLATE,<车牌>,<置信度>,<帧号> 只走真节点口(UART1); 电脑口看结果块 | 电脑要发命令先解锁: AT+CTRL=PC (命令表 AT+HELP)");
}

void node_link_poll(void)
{
    uint8_t buf[NL_RX_CHUNK];

#if NODE_LINK_USE_UART1
    for (int round = 0; round < NL_POLL_ROUND; round++) {
        const int n = uart_read_bytes(NL_UART, buf, sizeof(buf), 0);
        if (n <= 0) break;
        nl_feed(NL_SRC_NODE, buf, n);
    }
#endif

    for (int round = 0; round < NL_POLL_ROUND; round++) {
        const int n = nl_read_stdin(buf, sizeof(buf));
        if (n <= 0) break;
        nl_feed(NL_SRC_PC, buf, n);
    }
}

void node_link_report_plate(const char *plate, bool valid, int conf_pct, int frame_id)
{
    nl_set_last(plate, valid, conf_pct, frame_id);
    nl_send_plate(valid, s_last_plate, s_last_conf, s_last_frame, false);
}

/*
 * P5.74: 同步"现在生效的图片档位" —— 非 0 时每条 $PLATE 尾巴上会多一个 ",img=<档位>" 当提醒。
 *   主固件在 img_mode_apply() 里调; 传 0 表示没在发图, 什么都不附。
 */
void node_link_set_img_hint(int img_mode)
{
    s_img_hint = (img_mode > 0) ? img_mode : 0;
}

void node_link_set_extra_handler(node_link_extra_cmd_cb_t cb)
{
    s_extra_cb = cb;
}

uint32_t node_link_uplink_count(void)
{
    return s_up_plate;
}