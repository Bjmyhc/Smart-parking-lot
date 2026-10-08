# tools —— 脚本索引（命名约定 + 去哪找）

> 这里 130 多个脚本是**历史留档**，不是都要跑。按下面的约定找就行。
> 配套：训练工程在仓库外 `G:\All_Project\AI_Project\LPRNet_Pytorch`，用它的 venv 跑：
> `G:\All_Project\AI_Project\LPRNet_Pytorch\venv\Scripts\python.exe -X utf8 <脚本>`

## 命名约定（前任留下的，沿用）

| 前缀/后缀 | 含义 | 数量 |
| --- | --- | --- |
| 无前缀 | 常驻工具，可能还要再跑 | 18 |
| `diag_*` | 一次性诊断（查某次现象的原因） | 12 |
| `p5xx_*` / `p56x` / `p57x` | 按固件补丁号命名的一次性实验脚本 | 41 |
| `e2e_*` | 端到端链路实验 | 4 |
| `_*`（下划线开头） | **一次性/临时**，写完就用完，可视为归档 | 57 |
| `*_selftest.*` | 离线自检（回归用） | 1 py + 3 其它语言 |
| `out/` | 所有产物（模型、图表、日志） | 已被 .gitignore 忽略 |

## 常驻工具（要改模型/复算就找这些）

| 脚本 | 干什么 |
| --- | --- |
| `export_onnx_s3.py` | 训练 → ONNX（含与 PyTorch 的数值等价性 + 算子白名单体检） |
| `quant_espdl_s3.py` | ONNX → int8 esp-dl（`--calib-algorithm` / `--calib-dirs` / `--quant_type` / `--eval-n`） |
| `inspect_espdl.py` | `.espdl` 离线体检：算子是否注册、有无 0 维参数、参数数据是否齐 |
| `lprnet_s3_model.py` | 量化友好版网络定义（`build_lprnet_s3`），训练/微调/复算共用 |
| `finetune_s3.py` | 用本项目实拍素材微调（产出 r1~r5 那批权重） |
| `p600_calib_ab.py` | **量化对照实验**（校准算法 × 校准集 × w8a8/w8a16/w16a16，三套验证集打分） |
| `eval_ckpt.py` | 任意权重 × 三套验证集打分 + 按车牌/省份分解 + 错例 |
| `make_holdout.py` | 生成 33 张闭卷留出集（防止"开卷自夸"） |
| `prep_collect.py` / `pick_hard.py` | 实拍素材预处理与挑难题 |
| `locator_replay.py` / `locator_text_row.py` | **拿串口日志回放定位/裁剪算法**，复算框套得准不准 |
| `node_sim.py` | 节点仿真：用电脑发 AT 指令走一遍真实链路 |
| `dump_test_input.py` / `make_crop_probe.py` | 生成 `main/models/` 里那两个 27KB 探针文件 |

## 三套验证集（打分必看，别自己临时拼）

| 名称 | 位置 | 说明 |
| --- | --- | --- |
| 留出 33（闭卷） | `tools/out/holdout` | 模型没见过，唯一诚实的数字 |
| 实拍 336（开卷） | `G:\All_Project\AI_Project\BY串口助手\dist\saved\images` | 现场屏幕翻拍，含大量困难帧，float 上限本身只有 ~41% |
| 皖 200 | `LPRNet_Pytorch\data\official_val` | 通用数据集，防止过拟合到自己的场景 |

## 坑

- **别拿错 ONNX**：`out/r4_s3.onnx` 才是 r4 模型，`out/lprnet_s3.onnx` 是 r4 之前的旧导出。
- 很多脚本把 94×24、`(x-127.5)/128` 这套预处理**各写了一份**，改预处理口径要一起改
  （`quant_espdl_s3.preprocess` / `finetune_s3` / `eval_ckpt` / `lprnet_s3_model`）。
- `p56x`/`p57x` 那一串是省字小模型的训练链（合成预训练 → 真实素材微调 → 量化 → 4 折验证），
  中间靠 `exec(open(...).read())` 复用前一个脚本的网络定义，**改名字要一起改**。
- `_edit/` 是当时"补丁怎么打进去"的施加记录（ps1 + 输出文本），仅作留档。