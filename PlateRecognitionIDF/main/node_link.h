/*
 * node_link —— 摄像头(ESP32-S3) -> 节点单片机 的那条链路 (P5.80)
 *
 * 为什么有这条链路
 *   摄像头原来是"独立个体": 自己拍、自己认、自己上报云平台。
 *   现在改成"节点的一个外设": 摄像头只把车牌文本交给节点, 由节点经 LoRa -> 网关 上报。
 *   这条链路传文本(车牌 $PLATE 等), 也传图 —— S5 起节点用 AT+THUMB 取 94×24 二值缩略图,
 *   摄像头按 $IMGD/$IMGB/$IMGE 三行回传(282B), 节点再经 LoRa 0xF2 两分包转给网关。
 *   大图(JPEG)仍是调试手段, 走控制台口。
 *
 * 物理层选择 (只改下面这一个宏, 上层一行都不用动)
 *   NODE_LINK_USE_UART1 = 0 : 复用控制台串口。
 *       现在的现实: 手上没有节点板, 用电脑串口助手当"假节点" —— 发 AT 命令进来, 收结果块/未识别原因。
 *       零额外硬件, 插着现在这根 USB 线就能调。
 *   NODE_LINK_USE_UART1 = 1 : 走 UART1, 现用 **TX=GPIO47 / RX=GPIO48 @115200** (P5.80)。
 *       这两个脚就是板上丝印写着 SDA / SCL 的那两个 —— ESP32 的 GPIO 矩阵允许把 UART 映射到
 *       任意空闲 GPIO, 丝印只是给人看的标签, 跟芯片无关。
 *       接法: 摄像头 TX(47) -> 节点 USART3 RX(PB11), 摄像头 RX(48) -> 节点 USART3 TX(PB10)。
 *       (把 48 当 RX 是为了避开少数 S3 开发板把 GPIO48 接板载 RGB 灯的情况 —— 那个脚做输入更干净。)
 *
 * 控制权 (P5.69) —— 一个开关 AT+CTRL, 两种模式, 每个模式一个"主人" + 一个只能发 AT+CTRL 的"旁观者":
 *
 *     |                    | 节点口 (UART1)   | 电脑口 (USB 控制台) |
 *     | 正常模式 (上电默认) | 能发全部命令     | 只能发 AT+CTRL / AT+IMG / AT+HELP / AT+INFO / AT+XXX? |
 *     | 调试模式            | 只能发 AT+CTRL   | 能发全部命令        |
 *
 *   为什么: 真节点接上以后两边是两条独立的线, 谁都能发命令必然打架 ——
 *   电脑发 AT+RUN 的同时节点也在等结果, 那一行 $PLATE 到底算谁的?
 *   干脆只让一个"主人"发命令, 另一个只能旁观(看得到日志, 发不了命令)。
 *   AT+CTRL 双向放行(旁观者一键抢回控制权); AT+HELP / AT+INFO / AT+XXX? 是只读的, 也不看控制权 (P5.79)。
 *   切换在帧间生效(主固件消费事件后才改行为), 不会打断正在跑的那一帧。
 *   不做自动切回、不做定时提醒 —— 上电恒为正常模式, 重启即归还。
 *
 * 镜像 (P5.70; P5.75 起常开, 没有开关)
 *   节点口收到的每条命令、以及发给节点的每一行($PLATE / 回复), 都抄一份到电脑口 ——
 *   真节点接上以后那条线是给节点自己看的, 电脑上什么都看不见, 调试只能靠这份抄本
 *   (节点侧同款范式: bsp_lora.c 发真帧 + 打一行人话摘要)。$PLATE 那行额外附一句人话。
 *   时期 A(只有一条 USB 线)两条口本来就是同一根线, 抄了就是同一句话打两遍 -> 那时自动不抄;
 *   P5.75: 去掉 AT+MIRROR 开关 —— 没有"要关掉"的场景, 一直开着(时期 B 抄, 时期 A 不抄)。
 *   P5.80: 每帧自动上行的 $PLATE 也纳入镜像 —— 手上没节点板时, 那根线上发出去没有
 *     全靠这行 [镜像] 才能看见 (以前它只走私下的"静默上行"通道, 电脑口完全看不到)。
 *
 *   时期 A (NODE_LINK_USE_UART1=0) 只有一条口, 那一条口算"电脑口":
 *   所以开机默认(正常模式)下电脑什么都发不了, 必须先来一条 AT+CTRL=PC 解锁, 才能发别的命令。
 *   这是刻意的 —— 现在就把权限逻辑跑起来, 以后插上真节点不用改语义。
 *
 * 上行 (摄像头 -> 节点)
 *   $PLATE,<车牌>,<置信度0-100>,<帧号>[,img=<档位>]\n
 *                         每处理完一帧发一行; 没认出来时车牌字段是 -;
 *                         发图时尾巴多一个 ",img=<档位>" 当提醒(节点只读前 3 个字段即可)
 *
 * 约定: 处理了一帧就一定有一行 $PLATE。认不出就发 $PLATE,-,<置信度>,<帧号> ——
 *   否则节点分不清"这帧没认出来"和"链路/摄像头死了", 只能干等超时。
 * P5.77: 上面这条只对"真节点口(UART1)"成立 —— 时期 A 节点口就是电脑口,
 *   每帧自动上行的 $PLATE 不再往那儿打 (电脑口看的是主固件的结果块/未识别块, 同一帧打两遍没意义);
 *   想核对线上格式就发 AT+TEST=<车牌> 或 AT+PUSH (这两条是"人主动要", 电脑口照打一份)。
 *
 * 从机规矩: 摄像头平时一个字都不发, 也不做心跳/定时上报 —— 主动发信会在节点侧造成
 *   计划外的时序。节点要探活就发 AT, 要看统计发 AT+INFO, 要结果发 AT+RUN。
 *
 * 下行 (节点 -> 摄像头)  行文本 AT 命令, 回复一行内容
 *   AT                  探活 -> OK
 *   AT+INFO             一眼看全 -> +INFO:<版本>,运行<秒>s,...,控制权,镜像,链路  再一行 +CFG:LOG/IMG/TRIG/PAD/PLATE 当前值
 *   AT+CTRL=NODE|PC     控制权归谁 (两种模式下都放行)
 *   AT+IMG=<0..3>       图片输出档 (只有电脑口能发; 正常模式下也放行 —— 图片只走电脑那条口)
 *   AT+HELP             命令表 (只读, 谁都能发)
 *   AT+PLATE            最近一次结果 -> +PLATE:<车牌>,<置信度>,<帧号>,<是否合法>
 *   AT+TEST=<车牌>      注入一个"测试车牌", 立刻走一遍上行 —— 没有真车牌也能验证节点侧解析
 *   AT+PUSH             把最近一次结果重推一遍
 *   其他 AT+XXX 交给 node_link_set_extra_handler() 注册的外挂分支 (主固件自己的调试命令)
 *
 * 回复规范 (P5.74 起):
 *   AT+XXX=<值>      设置 -> 成功回 OK; 失败回 ERR:<原因>
 *   AT+XXX           查询 -> 只回 +XXX:<当前值>, 不带任何解释
 *   AT+XXX?          看含义 -> 逐行列出这条命令每个取值代表什么 (只查文档, 不碰权限闸门)
 *   AT+PUSH / AT+TEST=<车牌>   动作 -> 回 OK (结果另以 $PLATE 行回来; 这两条是人主动要的, 电脑口也打一份)
 *   AT+RUN                    动作 -> **不回 OK, 也不另发一行**: 它的回复就是那一帧的结果 ——
 *                             节点口是 $PLATE 行(约 1 秒后; 发图时尾巴带 ",img=<档位>"),
 *                             电脑口是主固件的结果块; 没出结果时电脑口给一个"未识别"块(含原因)
 *   不带 = 单独发一律是"查询当前值", 绝不会被当成"设成 0"; 只有 AT+TEST 没有可查的值。
 *
 * 协议为什么这么简单
 *   这条线是点对点、几厘米、两个 3.3V TTL 直接对接, 干扰极小;
 *   短行文本 + 固定字段顺序足够, 先不上校验和。以后线缆加长/环境变脏, 再在行尾加 *XX 校验。
 */
#ifndef NODE_LINK_H
#define NODE_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_LINK_USE_UART1     1
#define NODE_LINK_UART_TX_GPIO  47
#define NODE_LINK_UART_RX_GPIO  48
#define NODE_LINK_UART_BAUD     115200

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 主固件自己的调试命令 (node_link.c 不认识的那些), 格式:
 *   verb = 去掉 "AT+" 并转成大写后的命令名 (例如 "LOG"), arg = '=' 之后的内容(可能为 NULL)
 *   填好 resp 即为回复内容; 不处理就把 resp 留空或填 "-", node_link 会回 ERROR。
 */
typedef void (*node_link_extra_cmd_cb_t)(const char *verb, const char *arg, char *resp, size_t resp_sz);

/* 初始化链路: 装驱动(或把 stdin 切成非阻塞) + 打一行身份日志。fw_version 只用于 AT+INFO */
void node_link_init(const char *fw_version);

/* 主循环每轮调一次; 非阻塞, 没有数据就立刻返回 */
void node_link_poll(void);

/* 报一次识别结果 —— 每调一次就发一行 $PLATE (认不出时车牌字段自动变成 -)。P5.77: 这是"每帧自动上行", 只走真节点口, 电脑口不打 */
void node_link_report_plate(const char *plate, bool valid, int conf_pct, int frame_id);

/****************************************************************************
 * node_link_send_thumb — S5: 把车牌缩略图按文本行推给节点(节点主动 AT+THUMB 时调)
 *
 * 行格式(节点口是纯文本行协议, 节点侧单行上限 128B, 故 hex 每行只装 60 字节):
 *   $IMGD,<len>,<imgNo>   起始行
 *   $IMGB,<hex>           数据行 ×N (每行 60B 原始数据的 hex, 6+120=126 字符)
 *   $IMGE,<crc16>         结束行, CRC16/MODBUS(4 位大写 hex)覆盖全部 <len> 字节
 * 节点校验 CRC 通过才缓存; imgNo 供节点透传给网关丢弃旧图残包。
 ****************************************************************************/
void node_link_send_thumb(const uint8_t *img, int len, int img_no);

/*
 * P5.74: 同步"现在生效的图片档位" —— 非 0 时每条 $PLATE 尾巴上多一个 ",img=<档位>" 当提醒。
 *   主固件在 img_mode_apply() 里调; 传 0 = 没在发图, 什么都不附。
 */
void node_link_set_img_hint(int img_mode);

/* 已经发出去几行 $PLATE —— 主固件用它判断"这一帧到底出没出结果" */
uint32_t node_link_uplink_count(void);

void node_link_set_extra_handler(node_link_extra_cmd_cb_t cb);

/* ==================== 控制权 (P5.69) ==================== */

/* 现在是不是"调试模式" —— true = 电脑是主人, false = 正常模式(节点是主人, 上电默认) */
bool node_link_ctrl_is_pc(void);

/* 当前控制权的命令名文案: "PC" / "NODE" */
const char *node_link_ctrl_str(void);

/*
 * 控制权刚刚被 AT+CTRL 切过 -> 消费掉这个事件并返回 true (只返回一次)。
 * 主固件拿它做模式相关的副作用: 切到正常模式时强制关掉串口图片 (发图是给电脑看的调试手段)。
 */
bool node_link_take_ctrl_event(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE_LINK_H */