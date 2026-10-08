# main/models —— 烧进固件的资源清单

这四个文件由 `main/CMakeLists.txt` 的 `embed_files` 逐个嵌进二进制（`target_add_aligned_binary_data`）。
**改动任何一个都要按下面的"换模型注意"重编。**

| 文件 | 大小 | SHA256(前16) | 是什么 | 谁生成 |
| --- | --- | --- | --- | --- |
| `lprnet_s3.espdl` | 484,976 B | `E8273D6BFE3811AA` | **主模型**：LPRNet，输入 94×24×3，输出 18 步 × 68 类，int8 | `tools/quant_espdl_s3.py` |
| `prov_s3.espdl` | 71,648 B | `01B3445157A96B91` | **省字复核小模型**：只看第一个字，输入 32×64×3，31 类，int8 | `tools/p567_quant_prov.py` |
| `probe_crop.bin` | 27,072 B | `1F9BFFB0B4CA0342` | 定位/裁剪复算用的探针输入（94×24×3 的 float32） | `tools/make_crop_probe.py` |
| `test_input.bin` | 27,072 B | `542FB414DA07AACA` | 数值对齐用的固定输入样本（PC 与端侧比对同源） | `tools/dump_test_input.py` |

## 当前这份的来历（可复现）

- 主模型权重 = **r4**：`tools/out/finetune/r4_lr1e4_nofreeze.pth`
  （基线权重是训练工程的 `weights/Final_LPRNet_model.pth`，r4 是在它上面用本项目实拍素材微调出来的）
- 量化配置 = **mse 校准 + w8a8(int8)**，校准集 = 实拍素材 + `data/test` 混合 96 张
- 等价产物在 `tools/out/q_r4_mse.espdl`（与本目录文件 SHA256 一致，可用来做"我烧的是哪份"的比对）

## 换模型注意（会静默失败的那个坑）

`target_add_aligned_binary_data` 生成的是 Ninja 产物、判定**只比 mtime**。直接覆盖 `.espdl` 后 build，
可能什么都没重新嵌进去 —— 症状是"开机横幅是新版本、模型还是旧的"。

正确做法：

1. 删掉 `build/<name>.espdl.S` 和 `build/esp-idf/main/CMakeFiles/__idf_main.dir/__/__/<name>.espdl.S.obj`；
2. `idf.py build`；
3. 用二进制包含校验确认（例如 Python 判断新 espdl 的字节序列出现在 `plate_recognition.bin` 里），
   别只看横幅版本号。

## 想重新生成一份模型？

训练工程在仓库外：`G:\All_Project\AI_Project\LPRNet_Pytorch`（用它的 venv 跑 `tools/` 里的脚本）。

```powershell
# 1) 导出 ONNX（注意产物名：r4 模型对应 r4_s3.onnx）
python export_onnx_s3.py
# 2) 量化（历史教训：官方默认 kl 会把归一化分母裁掉，精度暴跌，别用默认）
python quant_espdl_s3.py --calib-algorithm mse --calib-dirs "<实拍目录>,data/test" --eval-n 200
# 3) 体检（算子白名单 / 0 维参数 / 参数数据三道门禁）
python inspect_espdl.py out\r4_s3.espdl --list-nodes
# 4) 拷进本目录前先核对 int8 精度（三套验证集），再替换 + 重编
```

量化口径、三套验证集、以及"哪种校准算法值多少点"的实测结论见
`../docs/移植进度与根因分析.md` 第四十二节与第五十节。