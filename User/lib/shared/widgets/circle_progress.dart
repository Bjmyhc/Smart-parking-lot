import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';

class CircleProgress extends StatelessWidget {
  final double percentage;
  final double size;
  final Color color;
  final String? label;
  
  const CircleProgress({
    super.key, 
    required this.percentage, 
    this.size = 40, 
    required this.color,
    this.label,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: size,
      height: size,
      child: Stack(
        alignment: Alignment.center,
        children: [
          CircularProgressIndicator(
            value: percentage / 100,
            strokeWidth: 4,
            backgroundColor: AppColors.background,
            valueColor: AlwaysStoppedAnimation(color),
          ),
          Text(
            label ?? '${percentage.toInt()}%',
            style: TextStyle(
              fontSize: size * 0.28, 
              fontWeight: FontWeight.w600, 
              color: AppColors.textPrimary
            ),
          ),
        ],
      ),
    );
  }
}
