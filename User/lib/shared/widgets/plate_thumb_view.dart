import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../../core/models/alert_model.dart';
import '../../core/models/spot_model.dart';

/// ⭐ S5 车牌缩略图点阵视图: 绘制 PlateThumb 属性(94×24 二值位图的 base64).
///
/// 数据链路: 摄像头自适阈二值化(94×24, 1=白字 0=黑底) → 节点缓存 → LoRa 0xF2
/// 两分包 → 网关重组 base64(376 字符) → 平台 PlateThumb 属性 → 本组件解码绘制.
///
/// 位布局(与摄像头产图一致): 行优先、每行 94 bit 不断行、字节内高位在前,
/// 即第 i 个像素 = bytes[i >> 3] 的第 (7 - (i & 7)) 位.
/// base64 解码失败或长度不是 282 字节时渲染空(sized box), 绝不抛错.
class PlateThumbView extends StatefulWidget {
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

  @override
  State<PlateThumbView> createState() => _PlateThumbViewState();
}

/// ⭐ 解码结果按 base64 字符串缓存。
///
/// 列表/详情页每次轮询(约 5s)都会重建整棵子树: 若在 build 里 base64Decode
/// 376 个字符, 每次重建都会新分配一个 Uint8List, 既浪费又会让 CustomPaint
/// 认为"图变了"而逐像素重绘(94×24 里几百个白点 = 几百次 drawRect)。
/// 缓存后只有图真的换了才重新解码、重新绘制。
class _PlateThumbViewState extends State<PlateThumbView> {
  Uint8List? _bytes;

  @override
  void initState() {
    super.initState();
    _bytes = _decode(widget.base64Data);
  }

  @override
  void didUpdateWidget(covariant PlateThumbView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.base64Data != widget.base64Data) {
      _bytes = _decode(widget.base64Data);
    }
  }

  /// 解码失败或长度不是 282 字节 → null(渲染空, 绝不抛错)
  static Uint8List? _decode(String data) {
    try {
      final b = base64Decode(data);
      return b.length == PlateThumbView.imgBytes ? b : null;
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
        size: Size(
          PlateThumbView.imgWidth * widget.pixelSize,
          PlateThumbView.imgHeight * widget.pixelSize,
        ),
        painter: _PlateThumbPainter(
          bytes: bytes,
          pixelSize: widget.pixelSize,
        ),
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
      !identical(old.bytes, bytes) || old.pixelSize != pixelSize;
}

const Color _kThumbDialogPrimary = Color(0xFF0052D4);

/// ⭐ "当时图像"弹窗: 居中卡片展示工单生成时刻的车牌原图(94×24 点阵).
///
/// 归档页行尾入口与工单详情页入口共用。 图缺失(升级前旧工单/识别未出图)时
/// 显示"当时未记录图像"占位, 不报错; 点击遮罩或关闭按钮退出.
Future<void> showAlertThumbDialog(BuildContext context, AlertModel alert) {
  final String? thumb =
      (alert.plateThumb != null && alert.plateThumb!.isNotEmpty)
          ? alert.plateThumb
          : null;
  final created = alert.createdAt;
  final String timeLabel = created != null
      ? '${created.month.toString().padLeft(2, '0')}'
          '-${created.day.toString().padLeft(2, '0')} '
          '${created.hour.toString().padLeft(2, '0')}:'
          '${created.minute.toString().padLeft(2, '0')}'
      : '未知';

  return showDialog(
    context: context,
    barrierDismissible: true,
    builder: (ctx) => Dialog(
      backgroundColor: Colors.transparent,
      insetPadding: const EdgeInsets.symmetric(horizontal: 24),
      child: Container(
        constraints: const BoxConstraints(maxWidth: 400),
        padding: const EdgeInsets.fromLTRB(20, 18, 20, 16),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(16),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.18),
              blurRadius: 24,
              offset: const Offset(0, 8),
            ),
          ],
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            /* 标题行: 图标 + 车牌号 + 置信度 chip */
            Row(
              children: [
                const Icon(Icons.image_outlined,
                    size: 18, color: _kThumbDialogPrimary),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    alert.plateNumber,
                    style: const TextStyle(
                      fontSize: 17,
                      fontWeight: FontWeight.w700,
                      color: Color(0xFF1D2129),
                      letterSpacing: 1,
                    ),
                  ),
                ),
                if (alert.plateConfidence != null)
                  Container(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                    decoration: BoxDecoration(
                      color: _kThumbDialogPrimary.withValues(alpha: 0.1),
                      borderRadius: BorderRadius.circular(999),
                    ),
                    child: Text(
                      '置信度 ${alert.plateConfidence}%',
                      style: const TextStyle(
                        fontSize: 11,
                        color: _kThumbDialogPrimary,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
              ],
            ),
            const SizedBox(height: 14),
            /* 图: FittedBox 自适应缩放防小屏溢出; 无图给等高占位 */
            if (thumb != null)
              Center(
                child: FittedBox(
                  fit: BoxFit.scaleDown,
                  child: PlateThumbView(base64Data: thumb, pixelSize: 4),
                ),
              )
            else
              Container(
                height: 104,
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  color: const Color(0xFFF2F3F5),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: const Text(
                  '当时未记录图像',
                  style: TextStyle(fontSize: 13, color: Color(0xFF86909C)),
                ),
              ),
            const SizedBox(height: 14),
            /* 元信息: 车位 + 占用 + 检测时间 */
            Text(
              '${alert.spotId} · 占用 ${SpotModel.formatOccupiedDuration(alert.occupiedSec)} · $timeLabel',
              textAlign: TextAlign.center,
              style: const TextStyle(fontSize: 12, color: Color(0xFF86909C)),
            ),
            const SizedBox(height: 16),
            /* 关闭 */
            GestureDetector(
              onTap: () => Navigator.of(ctx).pop(),
              child: Container(
                height: 40,
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  color: _kThumbDialogPrimary.withValues(alpha: 0.08),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: const Text(
                  '关闭',
                  style: TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                    color: _kThumbDialogPrimary,
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    ),
  );
}
