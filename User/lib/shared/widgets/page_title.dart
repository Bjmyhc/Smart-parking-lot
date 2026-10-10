import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../../core/theme/app_colors.dart';

/// 4 个主页面(首页/车位/告警/我的)统一的大标题样式.
/// 字号/字重/颜色在此一处维护, 各页引用 PageTitle 即可同步生效.
///
/// ⭐ 长按触发隐藏功能(如车位页切换真实/模拟模式):
/// 按住标题满 [holdDuration](默认3秒)才触发, 期间标题下方显示进度条充能反馈;
/// 提前松手/滑出标题区域则复位不触发, 防止单击误触.
class PageTitle extends StatefulWidget {
  final String text;

  /// 可选: 标题长按回调 (如车位页长按3秒切换真实/模拟模式)
  final VoidCallback? onLongPress;

  /// 长按触发所需时长, 默认3秒
  final Duration holdDuration;

  const PageTitle(
    this.text, {
    super.key,
    this.onLongPress,
    this.holdDuration = const Duration(seconds: 3),
  });

  @override
  State<PageTitle> createState() => _PageTitleState();
}

class _PageTitleState extends State<PageTitle> with SingleTickerProviderStateMixin {
  late final AnimationController _progress = AnimationController(
    vsync: this,
    duration: widget.holdDuration,
    value: 0,
  );
  Timer? _holdTimer;
  Offset? _downPosition; // 按下点, 用于检测滑出标题区域

  static const _style = TextStyle(
    fontSize: 28,
    fontWeight: FontWeight.w400,
    color: AppColors.textPrimary,
  );

  @override
  void dispose() {
    _holdTimer?.cancel();
    _progress.dispose();
    super.dispose();
  }

  void _startHold(Offset position) {
    if (widget.onLongPress == null || _holdTimer != null) return;
    _downPosition = position;
    _progress.forward(from: 0);
    _holdTimer = Timer(widget.holdDuration, () {
      _holdTimer = null;
      _downPosition = null;
      _progress.value = 0;
      HapticFeedback.mediumImpact(); // 充能完成震感提示
      widget.onLongPress?.call();
    });
  }

  void _cancelHold() {
    _holdTimer?.cancel();
    _holdTimer = null;
    _downPosition = null;
    if (_progress.value > 0) _progress.reverse(); // 未满时松手 → 进度条回退
  }

  @override
  Widget build(BuildContext context) {
    final text = Text(widget.text, style: _style);
    if (widget.onLongPress == null) return text;

    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: (e) => _startHold(e.position),
      onPointerMove: (e) {
        // 手指滑出标题区域(>24px)视为放弃, 取消充能
        final start = _downPosition;
        if (start != null && (e.position - start).distance > 24) _cancelHold();
      },
      onPointerUp: (_) => _cancelHold(),
      onPointerCancel: (_) => _cancelHold(),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          text,
          // 进度条: 仅按住期间出现, 宽度 = 按住进度 × 标题区宽度, 左对齐充能;
          // 提前松手 reverse 回退至消失, 不占常驻布局空间
          AnimatedBuilder(
            animation: _progress,
            builder: (context, _) {
              final v = _progress.value;
              if (v <= 0) return const SizedBox.shrink();
              return Padding(
                padding: const EdgeInsets.only(top: 4),
                child: LayoutBuilder(
                  builder: (context, constraints) => SizedBox(
                    width: constraints.maxWidth * v,
                    height: 4,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        color: AppColors.primary,
                        borderRadius: BorderRadius.circular(2),
                      ),
                    ),
                  ),
                ),
              );
            },
          ),
        ],
      ),
    );
  }
}
