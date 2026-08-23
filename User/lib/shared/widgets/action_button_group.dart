import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_dims.dart';

class ActionButtonGroup extends StatelessWidget {
  final VoidCallback? onDispatch;
  final VoidCallback? onNotify;

  const ActionButtonGroup({
    super.key,
    this.onDispatch,
    this.onNotify,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: _buildButton(
            icon: Icons.notifications_outlined,
            label: '通知车主',
            color: AppColors.warning,
            onTap: onNotify,
          ),
        ),
        const SizedBox(width: 12),
        Expanded(
          child: _buildButton(
            icon: Icons.send_outlined,
            label: '派单',
            color: AppColors.primary,
            onTap: onDispatch,
          ),
        ),
      ],
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
        padding: const EdgeInsets.symmetric(vertical: 12),
        decoration: BoxDecoration(
          color: color.withOpacity(0.1),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(icon, color: color, size: 20),
            const SizedBox(width: 8),
            Text(
              label,
              style: TextStyle(
                fontSize: 14,
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
