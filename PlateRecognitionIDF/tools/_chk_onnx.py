# -*- coding: utf-8 -*-
import sys, numpy as np, onnxruntime as ort
sys.path.insert(0, r"G:\All_Project\AI_Project\LPRNet_Pytorch")
from data.load_data import CHARS
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
BIN  = ROOT + r"\main\models\test_input.bin"
blank = len(CHARS) - 1

def run(onnx, tag):
    x = np.fromfile(BIN, dtype="<f4").reshape(24, 94, 3).transpose(2, 0, 1)[None]
    s = ort.InferenceSession(onnx, providers=["CPUExecutionProvider"])
    o = s.run(None, {s.get_inputs()[0].name: x})[0]
    p = o[0] if o.ndim == 3 else o
    lab = [int(np.argmax(p[:, j])) for j in range(p.shape[1])]
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank: out.append(c)
        prev = c
    print("%-34s -> %s" % (tag, "".join(CHARS[i] for i in out)))

run(ROOT + r"\tools\out\lprnet_s3.onnx", "lprnet_s3.onnx (tools/out)")
run(ROOT + r"\tools\out\r4_s3.onnx",      "r4_s3.onnx")
