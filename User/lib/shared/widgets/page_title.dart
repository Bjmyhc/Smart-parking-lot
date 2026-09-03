import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';

/// 4 个主页面(首页/车位/告警/我的)统一的大标题样式.
/// 字号/字重/颜色在此一处维护, 各页引用 PageTitle 即可同步生效.
class PageTitle extends StatelessWidget {
  final String text;
  /// 可选: 标题点击回调 (如车位页点标题切换真实/模拟模式)
  final VoidCallback? onTap;

  const PageTitle(this.text, {super.key, this.onTap});

  @override
  Widget build(BuildContext context) {
    const style = TextStyle(
      fontSize: 28,
      fontWeight: FontWeight.w400,
      color: AppColors.textPrimary,
    );
    final child = Text(text, style: style);
    if (onTap == null) return child;
    return GestureDetector(
      onTap: onTap,
      behavior: HitTestBehavior.opaque,
      child: child,
    );
  }
}
