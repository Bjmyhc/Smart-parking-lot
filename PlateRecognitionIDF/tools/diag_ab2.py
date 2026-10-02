# -*- coding: utf-8 -*-
import os, sys, glob
import numpy as np, cv2, onnxruntime as ort
import torch
ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
LPRNET = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
sys.path.insert(0, os.path.join(ROOT,'tools')); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
CROPS='G:\\All_Project\\AI_Project\\BY串口助手\\dist\\saved\\images'
blank=len(CHARS)-1
def dec(a):
    a=np.squeeze(np.asarray(a,dtype=np.float32))
    if a.shape[0]!=len(CHARS): a=a.T
    lab=[int(np.argmax(a[:,j])) for j in range(a.shape[1])]
    out,prev=[],blank
    for c in lab:
        if c!=prev and c!=blank: out.append(c)
        prev=c
    return ''.join(CHARS[i] for i in out)
def prep(p):
    im=cv2.imdecode(np.fromfile(p,np.uint8),cv2.IMREAD_COLOR)
    im=cv2.resize(im,(94,24)).astype(np.float32); im=(im-127.5)*0.0078125
    return im.transpose(2,0,1)[None]
torch.set_grad_enabled(False)
net=build_lprnet_s3(lpr_max_len=8,class_num=len(CHARS),dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT,'tools','out','finetune','r4_lr1e4_nofreeze.pth'),map_location='cpu')); net.eval()
s_old=ort.InferenceSession(os.path.join(ROOT,'tools','out','lprnet_s3.onnx'),providers=['CPUExecutionProvider'])
files=[]
for d in sorted(os.listdir(CROPS)):
    p=os.path.join(CROPS,d)
    if os.path.isdir(p): files+=[(f,d.replace('-','')) for f in glob.glob(os.path.join(p,'*.jpg'))]
files+=[(f,'京Q06666') for f in sorted(glob.glob(os.path.join(CROPS,'IMG_20261001_1740*.jpg')))]
st={'old':{'prov':0,'full':0,'short':0},'r4':{'prov':0,'full':0,'short':0}}
for f,gt in files:
    a=prep(f)
    o=s_old.run(None,{s_old.get_inputs()[0].name:a})[0]
    r1=dec(o[0] if o.ndim==3 else o); r2=dec(net(torch.from_numpy(a)).numpy()[0])
    for k,t in (('old',r1),('r4',r2)):
        st[k]['full']+= (t==gt)
        st[k]['prov']+= (len(t)>0 and len(gt)>0 and t[0]==gt[0])
        st[k]['short']+= (len(t)<len(gt))
n=len(files)
print('样本 %d 张 (你的实拍 94x24 裁块)'%n)
for k in ('old','r4'):
    print('%-4s  整串全对 %3d (%.0f%%) | 省字对 %3d (%.0f%%) | 字符少一个 %3d'%(
        k,st[k]['full'],100*st[k]['full']/n,st[k]['prov'],100*st[k]['prov']/n,st[k]['short']))