import sys, numpy as np, torch
sys.path.insert(0, r"G:\All_Project\AI_Project\LPRNet_Pytorch")
sys.path.insert(0, r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools")
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.eval()
torch.set_grad_enabled(False)
y = net(torch.zeros(1, 3, 24, 94))
print("CHARS", len(CHARS), "out shape", tuple(y.shape))
