# PlateRecognitionIDF —— 摄像头端侧车牌识别（ESP32-S3）

**一句话**：这块 ESP32-S3 相机板**离线**（不联网、不调云 API）跑两个轻量 CNN，把车牌号认出来，
再通过串口交给节点单片机；同时支持电脑用 AT 指令触发拍照、调试和收图。

- **当前固件：P5.80**（开机横幅第一行会打出来；认版本只看它，别只看文件时间）
- **硬件**：ESP32-S3（8MB Flash + 8MB PSRAM）+ GC2145 摄像头，取图 VGA 640×480 RGB565
- **模型**：LPRNet 主模型（约 44 万参数，输入 94×24）+ 省字复核小模型（约 6 万参数，输入 32×64），
  都是 int8 的 esp-dl 格式，随固件一起烧进 Flash
- **和云端无关**：早期"拍照 → 上传百度 OCR → OneNET 上报"的方案在 `../PlateRecognition/`，
  本工程是它的端侧替代品

## 目录导航

| 路径 | 是什么 | 能不能动 |
| --- | --- | --- |
| `main/` | 固件源码：`main.cpp`（识别全流程 + AT 指令）、`node_link.c/h`（给节点单片机的串口链路） | 编译清单在 `main/CMakeLists.txt`，两条源文件写死，改名要同步改 |
| `main/models/` | **烧进固件的模型与二进制**，见 `main/models/README.md` | 换模型后要删 `build/*.espdl.S` 再编（见下方坑） |
| `tools/` | 训练→导出→量化→诊断的脚本，见 `tools/README.md` | 平铺 + `_` 前缀约定，详见该 README |
| `docs/` | 本模块文档（含 230KB 的《移植进度与根因分析》），见 `docs/README.md` | 旧版/备份在 `docs/archive/` |
| `components/` | 本地化的驱动组件（`esp32-camera`、`esp_jpeg`），离线构建用 | 一般不动 |
| `managed_components/` | 组件管理器下载的 `esp-dl`（3.1.5）等 | 一般不动 |
| `build/` | 构建产物（不跟踪） | 可整个删掉重编 |
| `sdkconfig` / `sdkconfig.defaults` / `partitions.csv` / `dependencies.lock` | 构建配置 | 改前先看 `docs` 第 24.4 节记录的环境坑 |

## 编译与烧录

标准命令（假设 ESP-IDF 环境正常）：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM10 flash monitor
```

⚠ **本机的坑（务必先看 `docs/移植进度与根因分析.md` 第 24.4 节）**：Espressif 各版本 venv 的 `pyvenv.cfg`
都指向一个起不来的 `Python312\python.exe`，直接 `idf.py build` 会卡在
`Unable to create process ...`。当时可用的绕法是"自带解释器新建 venv + `.pth` 指到 IDF 的 site-packages
+ `idf.py --no-ccache build`"，并同步改 `build/CMakeCache.txt` 里的 `PYTHON`。

其它约定：
- 固件里已把模型打进二进制（`target_add_aligned_binary_data`），烧录用常规 app 分区即可；
  历史上也做过"合并镜像从 0x0 一次烧"（`build/*_merged.bin`）。
- 串口调试波特率 **921600**（控制台），节点链路 `UART1 TX=GPIO47 / RX=GPIO48 @115200`。

## AT 指令

- 电脑侧：`../Doc/AT指令手册.md`
- 节点侧：`../Doc/AT指令手册-节点版.md`
- 固件内置：发 `AT+HELP` 列全部命令，`AT+INFO` 看版本/时长/各项当前值，`AT+XXX?` 看某条命令的取值含义

## 三个别踩的坑

1. **别拿错模型文件做实验**：`tools/out/r4_s3.onnx` 是 r4 模型；`tools/out/lprnet_s3.onnx` 是 **r4 之前**的
   旧导出（`tools/diag_model_ab.py` 里写着"旧模型(r4之前)"）。量化实验一律用 `r4_s3.onnx`。
2. **换模型后必须删构建产物**：`target_add_aligned_binary_data` 是 Ninja 产物、只比 mtime，覆盖 `.espdl`
   可能被判定"已最新"而跳过 → 症状是"横幅是新的、模型是旧的"。删掉
   `build/lprnet_s3.espdl.S` 与 `build/esp-idf/main/CMakeFiles/__idf_main.dir/__/__/lprnet_s3.espdl.S.obj` 再编，
   并用二进制包含校验确认。
3. **识别效果的上限是画质**：过曝/发白的帧基本必错（实测 `过曝>14%` 或 `锐度<32` 的帧全错），
   这不是模型或量化的问题。

## 现在能干什么 / 不能干什么（截至 2026-10-08）

能：正面或小角度、大小合适、光照正常的蓝底车牌，端侧 1~2 次/秒，正确率可以到能用的水平；
认不出时会给原因（画面里没有车牌色 / 有一块但不规整 / 模型读出的字不符合车牌格式）。

不能：脏帧（强反光、过曝、拍糊）、太远、太斜的牌；`S↔8`、`0↔6↔8` 这类字体混淆在脏帧上仍会错。
量化侧已做过对照实验（`docs` 第五十节）：现在的 `mse + int8` 已经是性价比最高点。