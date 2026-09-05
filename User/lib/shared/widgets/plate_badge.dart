import 'package:flutter/material.dart';

import '../../core/theme/app_colors.dart';

/// ⭐ 真实车牌配色组件: 按 OCR 返回的车牌颜色(中文)给车牌上底色.
///
/// - 绿色(新能源): 绿底白字
/// - 蓝色: 蓝底白字 (含默认渐变, 与原告警小票样式一致)
/// - 黄色: 黄底黑字 / 白色: 白底黑字 / 黑色: 黑底白字 / 红色: 红底白字 / 灰色: 灰底白字
/// - 无颜色信息: [blueIfUnknown]=true 走蓝渐变(告警小票), false 走中性表面(车位详情原样式)
///
/// 失败态("拍了没识别出")与置信度由调用方自行组合, 本组件只负责"已知车牌"的底色.
class PlateBadge extends StatelessWidget {
  final String text;
  final String? color;
  final bool blueIfUnknown;
  final double fontSize;
  final EdgeInsetsGeometry padding;
  final double radius;
  final double letterSpacing;
  final FontWeight fontWeight;
  final List<BoxShadow>? boxShadow;
  final double borderWidth;
  final Color borderColor;

  const PlateBadge({
    super.key,
    required this.text,
    this.color,
    this.blueIfUnknown = false,
    this.fontSize = 22,
    this.padding = const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
    this.radius = 8,
    this.letterSpacing = 2,
    this.fontWeight = FontWeight.w700,
    this.boxShadow,
    this.borderWidth = 0,
    this.borderColor = Colors.white,
  });

  /// (渐变起始/结束, 文字色)
  (List<Color>, Color) get _palette {
    switch (color?.trim()) {
      case '绿色':
        return (const [Color(0xFF0FA05C), Color(0xFF0A7A45)], Colors.white);
      case '蓝色':
        return (const [Color(0xFF1A6FFF), Color(0xFF0052D4)], Colors.white);
      case '黄色':
        return (const [Color(0xFFFFC83C), Color(0xFFF0A800)], const Color(0xFF3A2E00));
      case '白色':
        return (const [Color(0xFFF8F8FA), Color(0xFFE3E3E9)], AppColors.textPrimary);
      case '黑色':
        return (const [Color(0xFF3C3C40), Color(0xFF101014)], Colors.white);
      case '红色':
        return (const [Color(0xFFE5484D), Color(0xFFA32027)], Colors.white);
      case '灰色':
        return (const [Color(0xFF9AA0A6), Color(0xFF5F6368)], Colors.white);
      default:
        return blueIfUnknown
            ? (const [Color(0xFF1A6FFF), Color(0xFF0052D4)], Colors.white)
            : ([AppColors.surface, AppColors.surface], AppColors.textPrimary);
    }
  }

  @override
  Widget build(BuildContext context) {
    final (colors, fg) = _palette;
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: colors,
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
        ),
        borderRadius: BorderRadius.circular(radius),
        boxShadow: boxShadow,
        border: borderWidth > 0
            ? Border.all(color: borderColor, width: borderWidth)
            : null,
      ),
      child: Text(
        text,
        style: TextStyle(
          fontSize: fontSize,
          fontWeight: fontWeight,
          color: fg,
          letterSpacing: letterSpacing,
        ),
      ),
    );
  }
}
