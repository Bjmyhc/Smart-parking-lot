import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_dims.dart';

class StatusBadge extends StatelessWidget {
  final String text;
  final Color color;

  const StatusBadge({
    super.key,
    required this.text,
    required this.color,
  });

  factory StatusBadge.free() => const StatusBadge(text: '空闲', color: AppColors.success);
  factory StatusBadge.occupied() => const StatusBadge(text: '占用', color: AppColors.warning);
  factory StatusBadge.zombie() => const StatusBadge(text: '僵尸车', color: AppColors.danger);

  factory StatusBadge.pending() => const StatusBadge(text: '待处理', color: AppColors.danger);
  factory StatusBadge.dispatched() => const StatusBadge(text: '处理中', color: AppColors.warning);
  factory StatusBadge.resolved() => const StatusBadge(text: '已处理', color: AppColors.success);

  factory StatusBadge.online() => const StatusBadge(text: '在线', color: AppColors.success);
  factory StatusBadge.offline() => const StatusBadge(text: '离线', color: AppColors.textSecondary);

  factory StatusBadge.fromStatus(String status) {
    switch (status) {
      case 'free':
        return StatusBadge.free();
      case 'occupied':
        return StatusBadge.occupied();
      case 'zombie':
        return StatusBadge.zombie();
      case 'pending':
        return StatusBadge.pending();
      case 'dispatched':
        return StatusBadge.dispatched();
      case 'resolved':
        return StatusBadge.resolved();
      case 'online':
        return StatusBadge.online();
      case 'offline':
        return StatusBadge.offline();
      default:
        return const StatusBadge(text: '未知', color: AppColors.textSecondary);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: color.withOpacity(0.1),
        borderRadius: BorderRadius.circular(AppDims.radiusSmall),
        border: Border.all(color: color.withOpacity(0.3), width: 0.5),
      ),
      child: Text(
        text,
        style: TextStyle(fontSize: 12, fontWeight: FontWeight.w500, color: color),
      ),
    );
  }
}
