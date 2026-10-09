import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';

/// ⭐ S5 车牌缩略图点阵视图: 绘制 PlateThumb 属性(94×24 二值位图的 base64).
///
/// 数据链路: 摄像头自适阈二值化(94×24, 1=白字 0=黑底) → 节点缓存 → LoRa 0xF2
/// 两分包 → 网关重组 base64(376 字符) → 平台 PlateThumb 属性 → 本组件解码绘制.
///
/// 位布局(与摄像头产图一致): 行优先、每行 94 bit 不断行、字节内高位在前,
/// 即第 i 个像素 = bytes[i >> 3] 的第 (7 - (i & 7)) 位.
/// base64 解码失败或长度不是 282 字节时渲染空(sized box), 绝不抛错.
class PlateThumbView extends StatelessWidget {
  /// base64 编码的位图字符串 (PlateThumb 属性值)
  final String base64Data;

  /// 每个图像像素的显示尺寸(px), 显示宽 = 94 × pixelSize
  final double pixelSize;

  /// 位图宽/高(像素), 与协议 LORA_IMG_W/H 一致
  static const int imgWidth = 94;
  static const int imgHeight = 24;
  static const int imgBytes = imgWidth * imgHeight ~/ 8; // 282

  const PlateThumbView({
    super.key,
    required this.base64Data,
    this.pixelSize = 3,
  });

  Uint8List? get _bytes {
    try {
      final b = base64Decode(base64Data);
      return b.length == imgBytes ? b : null;
    } catch (_) {
      return null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final bytes = _bytes;
    if (bytes == null) return const SizedBox.shrink();
    return Container(
      padding: const EdgeInsets.all(6),
      decoration: BoxDecoration(
        color: const Color(0xFF0B0B0E),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.black26),
      ),
      child: CustomPaint(
        size: Size(imgWidth * pixelSize, imgHeight * pixelSize),
        painter: _PlateThumbPainter(bytes: bytes, pixelSize: pixelSize),
      ),
    );
  }
}

class _PlateThumbPainter extends CustomPainter {
  final Uint8List bytes;
  final double pixelSize;

  const _PlateThumbPainter({required this.bytes, required this.pixelSize});

  @override
  void paint(Canvas canvas, Size size) {
    final bg = Paint()..color = const Color(0xFF0B0B0E);
    final fg = Paint()..color = Colors.white;
    canvas.drawRect(Offset.zero & size, bg);
    const w = PlateThumbView.imgWidth;
    const h = PlateThumbView.imgHeight;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        final i = y * w + x;
        if (((bytes[i >> 3] >> (7 - (i & 7))) & 1) == 1) {
          canvas.drawRect(
            Rect.fromLTWH(x * pixelSize, y * pixelSize, pixelSize, pixelSize),
            fg,
          );
        }
      }
    }
  }

  @override
  bool shouldRepaint(_PlateThumbPainter old) =>
      old.bytes != bytes || old.pixelSize != pixelSize;
}
