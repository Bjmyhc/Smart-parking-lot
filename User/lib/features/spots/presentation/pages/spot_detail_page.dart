import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/custom_app_bar.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';

class SpotDetailPage extends StatefulWidget {
  final SpotModel spot;

  const SpotDetailPage({
    super.key,
    required this.spot,
  });

  @override
  State<SpotDetailPage> createState() => _SpotDetailPageState();
}

class _SpotDetailPageState extends State<SpotDetailPage> {
  SpotModel _liveSpot(ParkingProvider provider) {
    for (final s in provider.spots) {
      if (s.id == widget.spot.id) return s;
    }
    return widget.spot;
  }

  bool _hasAlert(SpotModel spot, int alertSec) =>
      (spot.isZombie || spot.isOccupied) && spot.occupiedHours * 3600 >= alertSec;

  String _formatDateTime(DateTime time) {
    String two(int v) => v.toString().padLeft(2, '0');
    return '${time.year}-${time.month}-${time.day} ${two(time.hour)}:${two(time.minute)}';
  }

  String _formatUpdatedAt(String? lastUpdated) {
    if (lastUpdated == null || lastUpdated.isEmpty) return '未知';
    final dt = DateTime.tryParse(lastUpdated);
    if (dt == null) return lastUpdated;
    return _formatDateTime(dt);
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final spot = _liveSpot(provider);

    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: CustomAppBar(
        title: spot.id,
        showBackButton: true,
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _buildHeader(spot),
            const SizedBox(height: AppDims.gapCard),
            _buildPlateInfo(spot),
            const SizedBox(height: AppDims.gapCard),
            _buildDeviceInfo(spot),
            if (_hasAlert(spot, provider.alertSec)) ...[
              const SizedBox(height: AppDims.gapCard),
              _buildAlertInfo(provider, spot),
            ],
            const SizedBox(height: 100),
          ],
        ),
      ),
    );
  }

  Widget _buildHeader(SpotModel spot) {
    final statusBadge = spot.isDisabledSpot
        ? StatusBadge.disabled()
        : spot.isOffline
            ? StatusBadge.offline()
            : spot.isFree
                ? StatusBadge.free()
                : spot.isOccupied
                    ? StatusBadge.occupied()
                    : StatusBadge.zombie();

    return CardContainer(
      child: Column(
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                spot.id,
                style: const TextStyle(
                  fontSize: 24,
                  fontWeight: FontWeight.w500,
                  color: AppColors.textPrimary,
                ),
              ),
              statusBadge,
            ],
          ),
          const SizedBox(height: 20),
          Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: spot.isOffline
                  ? AppColors.textSecondary.withValues(alpha: 0.05)
                  : AppColors.primary.withValues(alpha: 0.05),
              borderRadius: BorderRadius.circular(AppDims.radiusMedium),
            ),
            child: spot.isOffline
                ? Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Icon(Icons.cloud_off, color: AppColors.textSecondary, size: 28),
                      const SizedBox(width: 12),
                      Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text(
                            '设备已离线',
                            style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textSecondary),
                          ),
                          const SizedBox(height: 4),
                          Text(
                            '无法获取实时数据,请检查设备网络',
                            style: TextStyle(fontSize: 12, color: AppColors.textSecondary.withValues(alpha: 0.7)),
                          ),
                        ],
                      ),
                    ],
                  )
                : Row(
                    mainAxisAlignment: MainAxisAlignment.spaceAround,
                    children: [
                      _buildInfoItem(
                        icon: Icons.battery_full,
                        label: '电量',
                        value: '${spot.batteryLevel.round()}%',
                        color: spot.batteryLevel > 50 ? AppColors.success : AppColors.danger,
                      ),
                      Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.2)),
                      _buildInfoItem(
                        icon: Icons.signal_cellular_alt,
                        label: '信号',
                        value: '${spot.signalStrength}dBm',
                        color: AppColors.primary,
                      ),
                      Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.2)),
                      _buildInfoItem(
                        icon: Icons.timer,
                        label: '占用时长',
                        value: '${spot.occupiedHours}h',
                        color: spot.isZombie ? AppColors.danger : AppColors.warning,
                      ),
                    ],
                  ),
          ),
        ],
      ),
    );
  }

  Widget _buildInfoItem({
    required IconData icon,
    required String label,
    required String value,
    required Color color,
  }) {
    return Column(
      children: [
        Icon(icon, color: color, size: 24),
        const SizedBox(height: 8),
        Text(
          value,
          style: const TextStyle(
            fontSize: 16,
            fontWeight: FontWeight.w600,
            color: AppColors.textPrimary,
          ),
        ),
        const SizedBox(height: 4),
        Text(
          label,
          style: const TextStyle(
            fontSize: 12,
            color: AppColors.textSecondary,
          ),
        ),
      ],
    );
  }

  Widget _buildPlateInfo(SpotModel spot) {
    final plateNumber = spot.plateNumber ?? '暂无车牌信息';
    final isOccupied = spot.isOccupied || spot.isZombie;

    if (spot.isOffline) {
      return CardContainer(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              '车辆信息',
              style: TextStyle(
                fontSize: 16,
                fontWeight: FontWeight.w600,
                color: AppColors.textPrimary,
              ),
            ),
            const SizedBox(height: 16),
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(24),
              decoration: BoxDecoration(
                color: AppColors.textSecondary.withValues(alpha: 0.05),
                borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                border: Border.all(
                  color: AppColors.textSecondary.withValues(alpha: 0.2),
                ),
              ),
              child: Column(
                children: [
                  const Icon(Icons.help_outline, size: 56, color: AppColors.textSecondary),
                  const SizedBox(height: 16),
                  const Text(
                    '状态未知',
                    style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textSecondary),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    '设备离线,无法获取车辆信息',
                    style: TextStyle(fontSize: 14, color: AppColors.textSecondary.withValues(alpha: 0.7)),
                  ),
                ],
              ),
            ),
          ],
        ),
      );
    }

    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '车辆信息',
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 16),
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(24),
            decoration: BoxDecoration(
              color: isOccupied
                  ? AppColors.warning.withValues(alpha: 0.1)
                  : AppColors.success.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(AppDims.radiusMedium),
              border: Border.all(
                color: isOccupied
                    ? AppColors.warning.withValues(alpha: 0.3)
                    : AppColors.success.withValues(alpha: 0.3),
              ),
            ),
            child: Column(
              children: [
                Icon(
                  isOccupied ? Icons.directions_car : Icons.check_circle,
                  size: 56,
                  color: isOccupied ? AppColors.warning : AppColors.success,
                ),
                const SizedBox(height: 16),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                  decoration: BoxDecoration(
                    color: AppColors.surface,
                    borderRadius: BorderRadius.circular(8),
                  ),
                  child: Text(
                    plateNumber,
                    style: const TextStyle(
                      fontSize: 22,
                      fontWeight: FontWeight.w700,
                      letterSpacing: 2,
                    ),
                  ),
                ),
                const SizedBox(height: 12),
                Text(
                  isOccupied ? '车辆已占用该车位' : '车位当前空闲',
                  style: const TextStyle(
                    fontSize: 14,
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

  Widget _buildDeviceInfo(SpotModel spot) {
    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '设备信息',
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 16),
          _buildInfoRow('设备名称', spot.id),
          _buildInfoRow('设备ID', spot.id),
          _buildInfoRow('最后更新', _formatUpdatedAt(spot.lastUpdated)),
          _buildInfoRow('在线状态', spot.isDisabledSpot
              ? '已停用'
              : spot.isOffline
                  ? '离线'
                  : '在线'),
        ],
      ),
    );
  }

  Widget _buildInfoRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 10),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(
            label,
            style: const TextStyle(
              fontSize: 14,
              color: AppColors.textSecondary,
            ),
          ),
          Text(
            value,
            style: TextStyle(
              fontSize: 14,
              color: label == '在线状态' && value == '离线'
                  ? AppColors.textSecondary
                  : AppColors.textPrimary,
              fontWeight: FontWeight.w500,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildAlertInfo(ParkingProvider provider, SpotModel spot) {
    final status = provider.spotAlertStatus(spot.id);

    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '告警详情',
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 16),
          _buildInfoRow('告警类型', '僵尸车 (占用≥24h)'),
          _buildInfoRow('占用时长', '${spot.occupiedHours} 小时'),
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 10),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                const Text(
                  '处理状态',
                  style: TextStyle(fontSize: 14, color: AppColors.textSecondary),
                ),
                StatusBadge.fromStatus(status),
              ],
            ),
          ),
          if (spot.handlerName != null)
            _buildInfoRow('处理人', spot.handlerName!),
          if (spot.handledAt != null)
            _buildInfoRow('完成时间', _formatDateTime(spot.handledAt!)),
          _buildInfoRow('通知状态', spot.isNotified ? '已通知车主' : '未通知'),
        ],
      ),
    );
  }
}
