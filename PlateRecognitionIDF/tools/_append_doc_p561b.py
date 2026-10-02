# -*- coding: utf-8 -*-
"""_append_doc_p561b.py -- 订正第四十五节(改成"整段删除") + 追加第四十六节(串口卡顿排查)"""
import io, sys
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\移植进度与根因分析.md"
src = io.open(P, encoding="utf-8", newline="").read()

OLD = u"| 1 | `g_utrim_on` | 默认 `true` -> **`false`**（代码保留，BOOT 双击仍可现场开回来做 A/B） |"
NEW = u"| 1 | `roi_probe_utrim()` + `g_utrim_on` + `ROI_UTRIM_*` | **整段删除**（连 BOOT 双击那个开关一起去掉；双击改成\"复位图片输出\"） |"
if src.count(OLD) != 1:
    print("anchor1 count=%d ABORT" % src.count(OLD)); sys.exit(1)
src = src.replace(OLD, NEW, 1)

OLD2 = u"""预期（PC 复算）：**整串全对 44% -> 84%，首字（省）对 91%**。"""
NEW2 = u"""预期（PC 复算）：**整串全对 44% -> 84%，首字（省）对 91%**。

> 追加（同日）：用户要求\"既然它这么坏，直接删掉\"。于是把 `roi_probe_utrim()` 整个函数、
> `g_utrim_on` 开关、`ROI_UTRIM_LEFT_MAX` / `ROI_UTRIM_BAND_MIN` 两个常数、
> 以及 ROI 日志里的 `蓝带[..] 多吃[..]` 字段**全部删除**；BOOT 双击改成\"复位图片输出\"。
> 顺带每帧省掉 24x9 次双线性采样。产物 `build/p562_merged.bin`（横幅仍是 P5.61）。"""
if src.count(OLD2) != 1:
    print("anchor2 count=%d ABORT" % src.count(OLD2)); sys.exit(1)
src = src.replace(OLD2, NEW2, 1)

SEC = u"""

---

## 第四十六节 串口"动不动就卡着不动"—— 排查记录（2026-10-01 夜）

### 一、现象

用户：\"这个窗口发来数据，它动不动就卡着不动，不知道是串口助手的问题，还是固件的问题。\"

### 二、方法：造一把照妖镜

新增 `tools/_sniff.py`：**完全复刻 BY串口助手的二进制帧解析状态机**
（`$IMG,<len>` -> 读 len 字节 -> 读 4 字节 CRC32 -> 等 `$END`），但做了两件助手没做的事：

1. 每一块到达的字节都记时间戳（能算出\"空档\"分布）；
2. 自己校验 CRC 并统计，最后报出\"结束时停在哪个状态\"（失步与否）。

跑法：`python tools/_sniff.py COM10 921600 75 out/p562/sniff01`（会先复位单片机）。

### 三、结果（75 秒，固件 P5.61，图片模式 0）

| 指标 | 值 | 解读 |
| --- | --- | --- |
| 收到帧数 / CRC 通过 | **94 / 94（0 失败）** | 设备这 75 秒的帧格式**零错误** |
| 结束时状态 | `WAIT_HEADER` | 解析器没有卡在任何中间态，**没有失步** |
| 非日志控制字节 | 389 | 全是 ANSI 颜色转义（0x1B），正常 |
| 最大空档 | **1.282 s @ t=4.1 s** | 落在开机自检/摄像头预热那几秒 |
| 其余空档 | 0.44 ~ 0.52 s | 与 1.25 fps 的帧周期一致，是正常的帧间间隔 |
| 平均 JPEG | 3464 B（最大 3881） | 本次镜头对着暗处，图很小（对着车牌时是 10~14 KB） |

**结论：这 75 秒里设备没有停发、没有失步、没有坏帧。** 也就是说\"卡着不动\"在这一轮**没有复现成设备侧问题**。

### 四、但确实挖到三个真实弱点（都还没改）

| # | 位置 | 问题 | 会造成什么 |
| --- | --- | --- | --- |
| 1 | 助手 `bin_frame_queue(maxsize=8)` | 主线程忙时 `put(timeout=1)` 抛 `queue.Full`，代码里是 `except: pass` —— **静默丢帧，且不打任何日志** | 图片预览停住、日志照常滚 —— 看起来就是\"卡着不动\"，而且无从查证 |
| 2 | 助手解析器**没有任何超时** | 一旦流里少一个字节（USB 抖动、传输中途关端口），它就永远停在 `READING_BIN` 等那 `len` 个字节 | 整条图片链路**永久失联**；重启单片机也没用（它还在等那几个字节），必须重开端口 |
| 3 | 固件 `img_tx_raw_write()` | 用 `esp_rom_output_tx_one_char` **逐字节忙等**发图。13 KB 的图 = 144 ms 纯线速，这期间主循环什么都不能干 | 帧率被压到 ~1.2 fps，观感\"一顿一顿\" |

第 2 条最值得注意：它和用户早先的描述（\"按 BOOT 切完模式后毫无反应，必须重启\"、
\"设备重启助手才正常\"）在机理上是一致的。

### 五、下一步（待用户决定）

- **A（先做，代价小）**：把助手的 `bin_frame_queue` 改成\"丢最旧、并且把丢帧次数打到日志里\"；
  再给解析器加一个\"停在 READING_BIN 超过 N 秒就复位到 WAIT_HEADER\"的看门狗。
  ⚠ 用户跑的是**打包好的 exe**（`dist/`），改 `.py` 必须重新打包才生效。
- **B**：固件把逐字节忙等换成 UART 驱动 + 环形缓冲（CPU 不用等），帧率能上去。
- **C**：下次抓包时把镜头**对着车牌**（图 10~14 KB）再抓一次，看大图下会不会出现空档。
"""
if u"第四十六节" in src:
    print("ALREADY PRESENT")
else:
    io.open(P, "w", encoding="utf-8", newline="").write(src + SEC)
    print("OK updated+appended")
