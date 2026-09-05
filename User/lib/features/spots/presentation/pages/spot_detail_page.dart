import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/custom_app_bar.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../shared/widgets/plate_badge.dart';
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

  /// 是否有处理过程需要展示: 有告警或有任一阶段的处理记录
  bool _hasHandlingRecord(SpotModel spot) =>
      spot.alertCreatedAt != null ||
      spot.notifiedAt != null ||
      spot.dispatchedAt != null ||
      spot.handledAt != null;

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
            _buildPlateInfo(provider, spot),
            const SizedBox(height: AppDims.gapCard),
            _buildDeviceInfo(spot),
            if (_hasAlert(spot, provider.alertSec)) ...[
              const SizedBox(height: AppDims.gapCard),
              _buildAlertInfo(provider, spot),
            ],
            if (_hasHandlingRecord(spot)) ...[
              const SizedBox(height: AppDims.gapCard),
              _buildHandlingTimeline(spot),
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
                        value: SpotModel.formatOccupiedDuration(spot.actualOccupiedSec),
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

  /// 摄像头 Cam001 只与真实车位 Park001 关联 (与车牌补全规则一致): 真实/在线/未停用才给远程拍照入口.
  bool _isCameraPark001(SpotModel spot) =>
      spot.id == 'Park001' && spot.isReal && !spot.isOffline && !spot.isDisabledSpot;

  /// 远程拍照按钮点击: 调摄像头 TriggerCapture 服务, 识别结果由 3s 轮询自动刷新.
  Future<void> _onRemoteCapture(ParkingProvider provider) async {
    final reason = await provider.capturePlateRemote();
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(reason ?? '已触发远程拍照, 识别结果刷新后自动显示'),
        duration: const Duration(seconds: 2),
      ),
    );
  }

  Widget _buildPlateInfo(ParkingProvider provider, SpotModel spot) {
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
                _buildPlateBadgeArea(spot),
                const SizedBox(height: 12),
                Text(
                  isOccupied ? '车辆已占用该车位' : '车位当前空闲',
                  style: const TextStyle(
                    fontSize: 14,
                    color: AppColors.textSecondary,
                  ),
                ),
                /* ⭐ 拍照识别附加信息: 车牌颜色 / 识别置信度 / 识别时间 (有牌时显示) */
                if (spot.plateNumber != null && spot.plateNumber!.isNotEmpty) ...[
                  const SizedBox(height: 6),
                  SizedBox(
                    width: double.infinity,
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        _buildCaptureInfoLine(
                            Icons.palette_outlined, '车牌颜色', spot.plateColor ?? '未知'),
                        _buildCaptureInfoLine(
                            Icons.verified_outlined,
                            '识别置信度',
                            spot.plateConfidence != null
                                ? '${(spot.plateConfidence! * 100).toStringAsFixed(1)}%'
                                : '未知'),
                        if (spot.capturedAt != null)
                          _buildCaptureInfoLine(
                              Icons.schedule, '识别时间', spot.capturedAt!),
                      ],
                    ),
                  ),
                ],
                if (_isCameraPark001(spot)) ...[
                  const SizedBox(height: 16),
                  // 远程拍照入口: 车牌由摄像头 Cam001 抓拍识别, 服务化调用 TriggerCapture
                  SizedBox(
                    width: double.infinity,
                    child: OutlinedButton.icon(
                      onPressed: provider.capturingRemote
                          ? null
                          : () => _onRemoteCapture(provider),
                      icon: provider.capturingRemote
                          ? const SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            )
                          : const Icon(Icons.photo_camera_outlined, size: 18),
                      label: Text(
                        provider.capturingRemote ? '拍照识别中...' : '远程拍照',
                        style: const TextStyle(fontWeight: FontWeight.w600),
                      ),
                      style: OutlinedButton.styleFrom(
                        foregroundColor: AppColors.primary,
                        side: BorderSide(
                          color: AppColors.primary.withValues(alpha: 0.5),
                        ),
                        padding: const EdgeInsets.symmetric(vertical: 12),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                        ),
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }

  /// ⭐ 车牌展示区: 
  ///  - 有牌 → PlateBadge 真实配色 + 车牌下方识别置信度小字(按阈值着色)
  ///  - 拍过但没识别出 → 相机✕图标 + "已拍照, 未识别到车牌"
  ///  - 从未拍照 → 原白底大字兜底
  Widget _buildPlateBadgeArea(SpotModel spot) {
    final plate = spot.plateNumber;
    final conf = spot.plateConfidence;
    final hasPlate = plate != null && plate.isNotEmpty;
    if (hasPlate) {
      return Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          PlateBadge(text: plate, color: spot.plateColor),
          if (conf != null) ...[
            const SizedBox(height: 6),
            Text(
              '识别置信度 ${(conf * 100).toStringAsFixed(1)}%',
              style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.w500,
                color: _confidenceColor(conf),
              ),
            ),
          ],
        ],
      );
    }
    if (spot.captureFailed) {
      // ⭐ "拍了但没拍明白": 与"从未拍照"明确区分
      return const Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.no_photography_outlined, size: 42, color: AppColors.textSecondary),
          SizedBox(height: 8),
          Text(
            '已拍照, 未识别到车牌',
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w500,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      );
    }
    // 从未拍照 / 空车位 → 原白底兜底
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(8),
      ),
      child: const Text(
        '暂无车牌信息',
        style: TextStyle(
          fontSize: 22,
          fontWeight: FontWeight.w700,
          letterSpacing: 2,
        ),
      ),
    );
  }

  /// 置信度阈值着色: ≥90% 绿 / ≥70% 橙 / <70% 红
  Color _confidenceColor(double conf) {
    if (conf >= 0.9) return const Color(0xFF16A34A);
    if (conf >= 0.7) return const Color(0xFFF59E0B);
    return const Color(0xFFEF4444);
  }

  /// 拍照识别信息行 (车牌颜色 / 置信度 / 识别时间)
  Widget _buildCaptureInfoLine(IconData icon, String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        children: [
          Icon(icon, size: 15, color: AppColors.textSecondary),
          const SizedBox(width: 7),
          Text(
            label,
            style: const TextStyle(fontSize: 13, color: AppColors.textSecondary),
          ),
          const Spacer(),
          Text(
            value,
            style: const TextStyle(fontSize: 13, color: AppColors.textPrimary, fontWeight: FontWeight.w500),
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
          _buildInfoRow('占用时长', SpotModel.formatOccupiedDuration(spot.actualOccupiedSec)),
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

  /// 🆕 处理情况时间轴卡片: 展示从告警创建到处理完成的完整流程
  Widget _buildHandlingTimeline(SpotModel spot) {
    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '处理情况',
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 20),
          // 步骤1: 告警创建
          _buildTimelineStep(
            icon: Icons.warning_amber,
            title: '告警上报',
            subtitle: '检测到僵尸车，系统自动生成告警',
            time: spot.alertCreatedAt,
            done: spot.alertCreatedAt != null,
            isLast: false,
          ),
          // 步骤2: 车主通知
          _buildTimelineStep(
            icon: Icons.notifications_active,
            title: '通知车主',
            subtitle: spot.notifiedAt != null ? '已发送挪车提醒' : '等待通知',
            time: spot.notifiedAt,
            done: spot.notifiedAt != null,
            isLast: false,
          ),
          // 步骤3: 派单处理
          _buildTimelineStep(
            icon: Icons.assignment_turned_in,
            title: '工单派单',
            subtitle: spot.dispatchedAt != null && spot.handlerName != null
                ? '派单给 ${spot.handlerName}'
                : '等待派单',
            time: spot.dispatchedAt,
            done: spot.dispatchedAt != null,
            isLast: false,
          ),
          // 步骤4: 处理完成
          _buildTimelineStep(
            icon: Icons.check_circle,
            title: '处理完成',
            subtitle: spot.handledAt != null ? '僵尸车已处理完毕' : '处理中',
            time: spot.handledAt,
            done: spot.handledAt != null,
            isLast: true,
          ),
        ],
      ),
    );
  }

  /// 时间轴单步: 左侧圆形图标+连接线, 右侧标题/描述/时间
  Widget _buildTimelineStep({
    required IconData icon,
    required String title,
    required String subtitle,
    required DateTime? time,
    required bool done,
    required bool isLast,
  }) {
    final bgColor = done ? AppColors.success : AppColors.textSecondary.withValues(alpha: 0.15);
    final iconColor = done ? Colors.white : AppColors.textSecondary;
    final lineColor = done ? AppColors.success : AppColors.textSecondary.withValues(alpha: 0.2);
    final titleColor = done ? AppColors.textPrimary : AppColors.textSecondary;

    return SizedBox(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // 左侧: 竖线+圆形图标
          SizedBox(
            width: 32,
            child: Column(
              children: [
                Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: bgColor,
                    shape: BoxShape.circle,
                  ),
                  child: Icon(
                    done ? Icons.check : icon,
                    color: iconColor,
                    size: 18,
                  ),
                ),
                if (!isLast)
                  Container(
                    width: 2,
                    height: 48,
                    color: lineColor,
                    margin: const EdgeInsets.symmetric(vertical: 6),
                  ),
              ],
            ),
          ),
          const SizedBox(width: 14),
          // 右侧: 标题 + 副标题 + 时间
          Expanded(
            child: Padding(
              padding: EdgeInsets.only(top: 2, bottom: isLast ? 0 : 20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          title,
                          style: TextStyle(
                            fontSize: 15,
                            fontWeight: FontWeight.w600,
                            color: titleColor,
                          ),
                        ),
                      ),
                      if (time != null)
                        Text(
                          _formatDateTime(time),
                          style: const TextStyle(
                            fontSize: 12,
                            color: AppColors.textSecondary,
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(
                    subtitle,
                    style: TextStyle(
                      fontSize: 13,
                      color: done ? AppColors.textSecondary.withValues(alpha: 0.9) : AppColors.textSecondary,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
