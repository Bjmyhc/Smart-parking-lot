# -*- coding: utf-8 -*-
"""LPRNet 的 esp-dl 3.1.5 可部署版 (ESP32-S3)

为什么需要这个文件
------------------
esp-dl 3.1.5 支持的算子全集就是
    managed_components/espressif__esp-dl/dl/module/include/dl_module_creator.hpp
    -> register_dl_modules() 里注册的那一批。

原 LPRNet 前向里有三处算子不在该表内, esp-ppq 会原样导出成对应节点:

    f_pow  = torch.pow(f, 2)                  -> Pow        (未注册)
    f_mean = torch.mean(f_pow, dim=(1,2,3))   -> ReduceMean (未注册)
    logits = torch.mean(x, dim=2)             -> ReduceMean (未注册)

运行端不认识这些节点 -> dl::Model::load() 在
dl_model_base.cpp:170 调 fbs::FbsModel::get_operation_parameter() 时
解引用空指针, 触发 LoadProhibited 崩溃 (100% 复现)。

本文件的做法: 只换写法, 数值完全等价
------------------------------------
    Pow(x, 2)             -> x * x                                  [Mul]
    mean over (C, H, W)   -> GlobalAveragePool + Conv2d(1x1, w=1/C)  [GlobalAveragePool + Conv]
    mean over dim=2 (H)   -> AveragePool(kernel=(H,1)) + squeeze     [AveragePool + Squeeze]

说明:
  * GlobalAveragePool 只能压 H/W, 通道维靠权重恒为 1/C 的 1x1 卷积压掉。
    这就是分类网络里 "GAP -> 1x1 Conv" 的标准写法, esp-dl 原生支持,
    而且整条链上没有任何 NCHW/NHWC 布局切换算子 (没有 Reshape/Transpose),
    对 esp-ppq 的 NHWC 改写是最安全的形式。
  * 引入的算子 Mul / Div / GlobalAveragePool / AveragePool / Conv / Squeeze
    全部在 esp-dl 3.1.5 的注册表内。

权重兼容
--------
不新增/删除任何带参数的层, state_dict 与原版逐键一致, 可直接加载
weights/Final_LPRNet_model.pth。
"""

import torch
import torch.nn as nn
import torch.nn.functional as F


class ChannelSliceMaxPool2d(nn.Module):
    """nn.MaxPool3d(4D 输入) 的等价替换: 先通道抽样, 再对 H/W 做标准 2D 最大池化。

    原版把 (N,C,H,W) 交给 nn.MaxPool3d, PyTorch 按 (C,D,H,W) 解释,
    因 kernel 第 0 维恒为 1, 等价于"通道等间隔抽样 + 2D 池化"。
    导出 ONNX 时该算子是 3D 规格配 4D 输入, onnxruntime 会拒绝加载,
    所以这里显式写成 Slice + MaxPool2d。
    """

    def __init__(self, kernel_size, stride):
        super(ChannelSliceMaxPool2d, self).__init__()
        self.ch_stride = stride[0]
        self.pool = nn.MaxPool2d(kernel_size=kernel_size[1:], stride=stride[1:])

    def forward(self, x):
        if self.ch_stride > 1:
            x = x[:, ::self.ch_stride, :, :]
        return self.pool(x)


class LPRNetS3(nn.Module):
    """LPRNet 骨架 + 量化/部署友好改造。

    参数与 model.LPRNet.LPRNet 完全一致 (由 base_cls 传入构造好的实例),
    因此可以直接 load 原版权重。
    """

    def __init__(self, base):
        super(LPRNetS3, self).__init__()
        self.lpr_max_len = base.lpr_max_len
        self.phase = base.phase
        self.class_num = base.class_num
        self.backbone = base.backbone
        self.container = base.container

        # 池化层换成 ONNX 友好的等价形式 (无参数, state_dict 不受影响)
        for i, m in enumerate(self.backbone):
            if isinstance(m, nn.MaxPool3d):
                self.backbone[i] = ChannelSliceMaxPool2d(m.kernel_size, m.stride)

        # 通道均值用的 1x1 卷积权重 (1/C)。
        # 放在普通 dict 里而不是 buffer, 是为了让 state_dict 与原版保持逐键一致,
        # load_state_dict(strict=True) 仍然可用。
        self._mix_cache = {}

    def _channel_mix_weight(self, channels, ref):
        w = self._mix_cache.get(channels)
        if w is None:
            w = torch.full((1, channels, 1, 1), 1.0 / float(channels), dtype=torch.float32)
            self._mix_cache[channels] = w
        return w.to(device=ref.device, dtype=ref.dtype)

    def _global_mean(self, t):
        """[1,C,H,W] 全维均值 -> [1,1,1,1]

        GlobalAveragePool 压 H/W, 1x1 Conv(权重 1/C) 压通道。
        Conv 权重是常量, 量化后仍然精确 (同通道权重全相等)。
        """
        channel_mean = F.adaptive_avg_pool2d(t, 1)          # GlobalAveragePool -> [1,C,1,1]
        channels = int(channel_mean.shape[1])
        weight = self._channel_mix_weight(channels, channel_mean)
        return F.conv2d(channel_mean, weight)               # Conv -> [1,1,1,1]

    def forward(self, x):
        keep_features = list()
        for i, layer in enumerate(self.backbone.children()):
            x = layer(x)
            if i in [2, 6, 13, 22]:
                keep_features.append(x)

        global_context = list()
        for i, f in enumerate(keep_features):
            if i in [0, 1]:
                f = nn.AvgPool2d(kernel_size=5, stride=5)(f)
            if i in [2]:
                f = nn.AvgPool2d(kernel_size=(4, 10), stride=(4, 2))(f)
            f_square = f * f                                 # 原 torch.pow(f, 2)
            f_mean = self._global_mean(f_square)             # 原 torch.mean(f_pow, dim=(1,2,3), keepdim=True)
            f = torch.div(f, f_mean)
            global_context.append(f)

        x = torch.cat(global_context, 1)
        x = self.container(x)
        # 原 logits = torch.mean(x, dim=2): 对 H 整段平均, 再 squeeze 掉该维
        logits = F.avg_pool2d(x, kernel_size=(int(x.shape[2]), 1)).squeeze(2)
        return logits


def build_lprnet_s3(lpr_max_len=8, class_num=68, dropout_rate=0.5):
    """构造一个 eval 模式、结构就绪但未加载权重的 LPRNetS3。"""
    from model.LPRNet import LPRNet

    base = LPRNet(lpr_max_len, False, class_num, dropout_rate)
    return LPRNetS3(base).eval()
