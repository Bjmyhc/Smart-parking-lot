import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/models/operation_log_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';

/// 操作日志页 (极简风格: 卡片列表, 记录 App 内用户关键操作, 按时间倒序)
class OperationLogPage extends StatelessWidget {
  const OperationLogPage({super.key});

  /// 按日志类型映射图标与颜色.
  (IconData, Color) _typeStyle(String type) {
    switch (type) {
      case 'notify':
        return (Icons.notifications_outlined, AppColors.primary);
      case 'dispatch':
        return (Icons.send_outlined, AppColors.warning);
      case 'ota':
        return (Icons.system_update_alt, AppColors.success);
      case 'mode':
        return (Icons.swap_horiz, AppColors.textSecondary);
      default:
        return (Icons.article_outlined, AppColors.textSecondary);
    }
  }

  /// 时间格式化: 今天的显示 HH:mm, 其它显示 MM-dd HH:mm.
  String _formatTime(DateTime t) {
    final now = DateTime.now();
    final hh = t.hour.toString().padLeft(2, '0');
    final mm = t.minute.toString().padLeft(2, '0');
    final hm = '$hh:$mm';
    if (t.year == now.year && t.month == now.month && t.day == now.day) {
      return '今天 $hm';
    }
    final md =
        '${t.month.toString().padLeft(2, '0')}-${t.day.toString().padLeft(2, '0')}';
    return '$md $hm';
  }

  @override
  Widget build(BuildContext context) {
    final logs = context.watch<ParkingProvider>().operationLogs;

    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        elevation: 0,
        centerTitle: true,
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: AppColors.textPrimary),
          onPressed: () => Navigator.pop(context),
        ),
        title: const Text(
          '操作日志',
          style: TextStyle(
            color: AppColors.textPrimary,
            fontSize: 17,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 20),
          child: Column(
            children: [
              const SizedBox(height: 8),
              Expanded(
                child: logs.isEmpty
                    ? _buildEmpty()
                    : ListView.separated(
                        padding: const EdgeInsets.only(top: 8, bottom: 24),
                        itemCount: logs.length,
                        separatorBuilder: (_, __) => const SizedBox(height: 12),
                        itemBuilder: (context, index) =>
                            _buildLogItem(logs[index]),
                      ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  /// 空态提示 (黄金分割定位: 内容中心在内容区距顶 38.2% 处).
  Widget _buildEmpty() {
    return Align(
      alignment: const Alignment(0, -0.236),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            Icons.history,
            size: 56,
            color: AppColors.textSecondary.withValues(alpha: 0.4),
          ),
          const SizedBox(height: 12),
          const Text(
            '暂无操作记录',
            style: TextStyle(
              fontSize: 14,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      ),
    );
  }

  /// 单条日志卡片: 类型图标 + 标题/详情 + 时间.
  Widget _buildLogItem(OperationLog log) {
    final (icon, color) = _typeStyle(log.type);

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.05),
            blurRadius: 12,
            offset: const Offset(0, 3),
          ),
        ],
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(10),
            ),
            child: Icon(icon, color: color, size: 22),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: Text(
                        log.title,
                        style: const TextStyle(
                          fontSize: 15,
                          fontWeight: FontWeight.w600,
                          color: AppColors.textPrimary,
                        ),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Text(
                      _formatTime(log.createdAt),
                      style: const TextStyle(
                        fontSize: 11,
                        color: AppColors.textSecondary,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 6),
                Text(
                  log.spotId != null ? '${log.spotId} · ${log.detail}' : log.detail,
                  style: const TextStyle(
                    fontSize: 12,
                    height: 1.4,
                    color: AppColors.textSecondary,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
