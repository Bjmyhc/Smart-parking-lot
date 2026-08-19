import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_dims.dart';

class ActionButtonGroup extends StatelessWidget {
  final VoidCallback? onDispatch;
  final VoidCallback? onNotify;
  final VoidCallback? onResolve;

  const ActionButtonGroup({
    super.key,
    this.onDispatch,
    this.onNotify,
    this.onResolve,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: AppColors.background,
        borderRadius: BorderRadius.circular(AppDims.radiusMedium),
      ),
      child: Row(
        children: [
          Expanded(
            child: _buildButton(
              icon: Icons.send_outlined,
              label: '派单',
              color: AppColors.primary,
              onTap: onDispatch,
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: _buildButton(
              icon: Icons.notifications_outlined,
              label: '通知',
              color: AppColors.warning,
              onTap: onNotify,
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: _buildButton(
              icon: Icons.check_circle_outlined,
              label: '处置',
              color: AppColors.success,
              onTap: onResolve,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildButton({
    required IconData icon,
    required String label,
    required Color color,
    VoidCallback? onTap,
  }) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 10),
        decoration: BoxDecoration(
          color: color.withOpacity(0.1),
          borderRadius: BorderRadius.circular(AppDims.radiusSmall),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, color: color, size: 20),
            const SizedBox(height: 4),
            Text(
              label,
              style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.w600,
                color: color,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
