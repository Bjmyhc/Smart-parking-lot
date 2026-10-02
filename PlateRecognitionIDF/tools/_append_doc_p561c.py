# -*- coding: utf-8 -*-
"""_append_doc_p561c.py -- 第四十六节 §五 换成"实际做了什么"+B 的结论"""
import io, sys
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\移植进度与根因分析.md"
src = io.open(P, encoding="utf-8", newline="").read()
OLD = u"""### 五、下一步（待用户决定）

- **A（先做，代价小）**：把助手的 `bin_frame_queue` 改成\"丢最旧、并且把丢帧次数打到日志里\"；
  再给解析器加一个\"停在 READING_BIN 超过 N 秒就复位到 WAIT_HEADER\"的看门狗。
  ⚠ 用户跑的是**打包好的 exe**（`dist/`），改 `.py` 必须重新打包才生效。
- **B**：固件把逐字节忙等换成 UART 驱动 + 环形缓冲（CPU 不用等），帧率能上去。
- **C**：下次抓包时把镜头**对着车牌**（图 10~14 KB）再抓一次，看大图下会不会出现空档。
"""
NEW = u"""### 五、A 已实施（助手侧三处修补）

改的是 `BY串口助手/serial_img_tool.py`，改了 5 个锚点（改前逐处断言唯一性，对不上就整体放弃）：

| 编号 | 改动 | 解决的洞 |
| --- | --- | --- |
| A1 | `_read_loop` 里新增**收帧看门狗**：卡在非 `WAIT_HEADER` 状态且 3 秒无新字节 -> 自动复位解析器，并往日志打一行 `[助手] 收帧超时: ...` | 以前一旦少一个字节就**永久失联**（重启单片机也没用），这是最像\"卡着不动\"的那条 |
| A2 | `bin_frame_queue` 满时从 `except: pass`（静默丢帧）改成**丢最旧、留最新**，并累计丢帧数 | 预览停在旧帧、日志照滚 |
| A3 | 丢帧数 + CRC 失败数**限流上报**（3 秒一次，走 `[诊断]` 日志） | 以前这两类事件在\"预览\"开关关掉时**完全静默**，无从查证 |

打包：`python -m PyInstaller "BY串口助手.spec" --distpath dist_new`（用户系统 Python 3.12 + PyInstaller 6.10）。
`dist/BY串口助手.exe` 已备份成 `dist/BY串口助手.exe.bak-20261001`。
**验证方式**：onefile 的 PYZ 是压缩的，直接搜字节搜不到 —— 所以写脚本把 CArchive 里的主编译模块
`marshal.loads` 出来递归遍历 `co_consts`，确认 `收帧超时` / `图片队列丢帧累计` / `CRC 校验失败累计` 三个字面量都在。

### 六、B 的结论：**不做**（用户猜\"只有好处没有坏处\"，实测不成立）

原想法：把 `img_tx_raw_write()` 的逐字节忙等换成 UART 驱动 + 环形缓冲，让 CPU 不等。

**问题在于：逐字节忙等根本不占额外时间。** 13 KB 在 921600 上是 144 ms 的**纯线速**（128 字节 FIFO
按 11.1 us/字节排空）；忙等和 `uart_write_bytes` 的**墙钟时间完全一样**，区别只是 CPU 在等的时候
能不能去干别的。而这一版固件主循环只有一件事要做，所以：

- 收益：帧周期 0.80 s 里 TX 占 144~150 ms ≈ **18%**，但这 18% 换成驱动**一秒都省不下来**；
- 风险：一旦驱动缓冲和 `esp_rom_output_tx_one_char` 两条写路径混用，日志会**插进 JPEG 中间**
  -> CRC 失败 -> 在没打 A1 补丁之前就是**永久失步**（正是要修的那个现象）。要安全做必须把
  全部输出（含 ESP_LOG）统一走驱动，改动面比看起来大。

真要提帧率，杠杆在别处（推理 398 ms 是死的）：把发图改成**每 2 帧发 1 张**，每帧省下约 150 ms，
帧周期 0.80 -> 0.65 s（约 +19%），零风险，代价是预览变成 0.6 fps。要不要做由用户定。

### 七、C 暂缓

用户认为不必再抓一次大图包。已把 `tools/_sniff.py` 留在仓库里，随时可跑。
"""
if src.count(OLD) != 1:
    print("anchor count=%d ABORT" % src.count(OLD)); sys.exit(1)
io.open(P, "w", encoding="utf-8", newline="").write(src.replace(OLD, NEW, 1))
print("OK")
