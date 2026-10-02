# -*- coding: utf-8 -*-
"""p567: 省字模型 esp-dl int8 量化 (prov_final.onnx -> prov_s3.espdl) + PC 端 int8 精度体检.

校准集 = 真实省字 patch (245 张, 目标域) + 每省 8 张合成 (把 31 类的激活范围补齐).
量化算法固定 minmax —— quant_espdl_s3.py 的教训: kl 会把归一化分母裁掉, 精度暴跌.

用法:
    python p567_quant_prov.py
"""
import os, sys, glob, random
import numpy as np, cv2, torch, torch.nn.functional as F

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
ONNX = os.path.join(OUT, "prov_final.onnx")
ESPDL = os.path.join(ROOT, "tools", "out", "prov_s3.espdl")
W, H = 32, 64

src = open(os.path.join(ROOT, "tools", "p565f_train2.py"), encoding="utf-8").read()
ns = {}
exec(src[:src.index("real = []")], ns)
render, augment, to_t, PROV = ns["render"], ns["augment"], ns["to_t"], ns["PROV"]

def load(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

real = []
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            real.append((im, PROV.index(ch)))
print("真实 %d 张" % len(real))

rng = random.Random(4242)
calib = [to_t(im) for im, _ in real]
for i, ch in enumerate(PROV):          # 每省 8 张合成, 用一个中等强度增广
    for _ in range(8):
        calib.append(to_t(augment(render(ch, rng), rng)))
print("校准集 %d 张 (真实 %d + 合成 %d)" % (len(calib), len(real), len(calib) - len(real)))

from torch.utils.data import DataLoader
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx

setting = QuantizationSettingFactory.espdl_setting()
setting.quantize_activation_setting.calib_algorithm = "minmax"

graph = espdl_quantize_onnx(
    onnx_import_file=ONNX,
    espdl_export_file=ESPDL,
    calib_dataloader=DataLoader(dataset=calib, batch_size=8, shuffle=False),
    calib_steps=len(calib),
    input_shape=[1, 3, H, W],
    target="esp32s3",
    num_of_bits=8,
    collate_fn=lambda b: b.to("cpu"),   # DataLoader 默认 collate 已经把 tensor 列表 stack 好了
    setting=setting,
    device="cpu",
    error_report=True,
    skip_export=False,
    export_config=True,
    verbose=0,
)
print("量化完成 -> %s (%.1f KB)" % (ESPDL, os.path.getsize(ESPDL) / 1024.0))

# 输入/输出张量的 scale (板上固件要用 exponent 搭归一化 LUT)
import math
for nm in list(graph.inputs) + list(graph.outputs):
    var = graph.variables[nm]
    sc = None
    try:
        sc = graph.quantize_config[nm].scale
    except Exception:
        pass
    if sc:
        print("%s %s: shape=%s scale=%.8g exponent=%d dtype=%s"
              % ("输入" if nm in graph.inputs else "输出", nm, var.shape, sc,
                 int(round(math.log2(sc))), var.dtype))
    else:
        print("%s %s: shape=%s (没拿到 scale) dtype=%s"
              % ("输入" if nm in graph.inputs else "输出", nm, var.shape, var.dtype))

# ---- PC 端 int8 体检: 量化图 (TorchExecutor) vs float ----
from esp_ppq.executor import TorchExecutor
import onnxruntime as ort

exe = TorchExecutor(graph=graph, device="cpu")
sess = ort.InferenceSession(ONNX, providers=["CPUExecutionProvider"])
iname = sess.get_inputs()[0].name
ys = np.array([i for _, i in real])
fa, qa = [], []
for im, _ in real:
    x = to_t(im).unsqueeze(0)                       # [1,3,64,32]
    fl = sess.run(None, {iname: x.numpy()})[0]
    q = exe.forward(inputs=[x])
    if isinstance(q, (list, tuple)):
        q = q[0]
    q = q.detach().cpu().numpy() if isinstance(q, torch.Tensor) else np.asarray(q)
    fa.append(int(fl.reshape(-1).argmax()))
    qa.append(int(q.reshape(-1).argmax()))
fa, qa = np.array(fa), np.array(qa)

print("")
print("=" * 74)
print("int8 体检 (全部 %d 张真实省字 patch; 模型训练时见过它们 -> 看的是量化损伤, 不是泛化)" % len(real))
print("=" * 74)
print("float top-1: %.1f%% (%d/%d)" % (100.0 * (fa == ys).mean(), int((fa == ys).sum()), len(ys)))
print("int8  top-1: %.1f%% (%d/%d)" % (100.0 * (qa == ys).mean(), int((qa == ys).sum()), len(ys)))
print("float/int8 判定一致率: %.1f%% (%d/%d)" % (100.0 * (fa == qa).mean(), int((fa == qa).sum()), len(ys)))
for i in np.where(fa != qa)[0][:20]:
    print("   不一致: float %s -> int8 %s" % (PROV[fa[i]], PROV[qa[i]]))