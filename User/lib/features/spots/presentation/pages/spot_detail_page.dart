import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/custom_app_bar.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../shared/widgets/plate_badge.dart';
import '../../../../shared/widgets/plate_thumb_view.dart';
import '../../../../shared/widgets/plate_input_dialog.dart';
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
  String? _fwVersion; // 节点固件版本 (进页异步拉取一次, 设备信息行展示)
  bool _fwLoading = false;
  bool _propsExpanded = false; // 物模型属性区默认收起, 点底部箭头展开

  @override
  void initState() {
    super.initState();
    _loadFwVersion();
  }

  Future<void> _loadFwVersion() async {
    _fwLoading = true;
    // spot.id 即 OneNET 设备名 (Park001 风格), 与 OTA 读版本接口的 deviceName 一致
    final v = await context.read<ParkingProvider>().getOtaNodeVersion(widget.spot.id);
    if (!mounted) return;
    setState(() {
      _fwLoading = false;
      _fwVersion = v;
    });
  }

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

  /// 摄像头挂在节点 Park001 上 (车牌随节点属性上报): 真实/在线/未停用才给远程拍照入口.
  /// 摄像头自身离线由 SpotModel.cameraOnline 判定 → 按钮置灰.
  bool _isCameraPark001(SpotModel spot) =>
      spot.id == 'Park001' && spot.isReal && !spot.isOffline && !spot.isDisabledSpot;

  /// 远程拍照按钮点击: 调【节点】TriggerCapture 服务(节点再触发摄像头).
  /// 按钮在整个"触发 + 取牌"期间保持"拍照识别中..."(provider.capturingRemote), 车牌取到即直接显示, 全程不弹窗;
  /// 只有真失败(节点忙/离线/冷却中)才弹原因.
  Future<void> _onRemoteCapture(ParkingProvider provider) async {
    final reason = await provider.capturePlateRemote();
    if (!mounted) return;
    if (reason == null || reason.isEmpty) return; // 成功/拍到未识别: 卡片已直接显示, 不弹提示
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(reason), duration: const Duration(seconds: 2)),
    );
  }

  Widget _buildPlateInfo(ParkingProvider provider, SpotModel spot) {
    final isOccupied = spot.isOccupied || spot.isZombie;
    /* 空闲车位默认不允许远程拍照(违背逻辑), 需在 策略配置→拍照策略 开启"空闲车位可拍照" */
    final freeBlocked = spot.isFree && !provider.policy.allowCaptureWhenFree;

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
                  /* ① 结果层: 当前生效的识别结果本体 (badge+修正角标 / 未识别 / 占位) */
                  _buildPlateBadgeArea(spot),
                  const SizedBox(height: 12),
                  Text(
                    isOccupied ? '车辆已占用该车位' : '车位当前空闲',
                    style: const TextStyle(
                      fontSize: 14,
                      color: AppColors.textSecondary,
                    ),
                  ),
                  /* ② 证据组: 识别依据 (摄像头原图 → 置信度 → 时间),
                   *    与结果上下紧邻, 视线垂直下移即可对照 */
                  if (spot.hasPlate) ...[
                    if (spot.hasPlateThumb) ...[
                      const SizedBox(height: 14),
                      PlateThumbView(base64Data: spot.plateThumb!),
                      const SizedBox(height: 4),
                      const Text(
                        '识别依据 · 摄像头原图',
                        style: TextStyle(fontSize: 11, color: AppColors.textSecondary),
                      ),
                    ],
                    const SizedBox(height: 8),
                    SizedBox(
                      width: double.infinity,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          _buildCaptureInfoLine(
                            Icons.verified_outlined,
                            '识别置信度',
                            spot.plateConfidence != null ? '${spot.plateConfidence}%' : '未知',
                            valueColor: spot.plateConfidence != null
                                ? _confidenceColor(spot.plateConfidence!)
                                : null,
                          ),
                          if (spot.plateUpdatedAt != null && spot.plateUpdatedAt!.isNotEmpty)
                            _buildCaptureInfoLine(
                                Icons.schedule, '识别时间', _formatUpdatedAt(spot.plateUpdatedAt)),
                        ],
                      ),
                    ),
                  ],
                  /* ③ 动作组: 修正与拍照横排一行 (各自存在才占位; 空车位无动作) */
                  if (isOccupied || _isCameraPark001(spot)) ...[
                    const SizedBox(height: 16),
                    Row(
                      children: [
                        if (isOccupied)
                          Expanded(
                            child: OutlinedButton.icon(
                              onPressed: () => _showPlateEditDialog(spot),
                              icon: const Icon(Icons.edit_outlined, size: 18),
                              label: Text(
                                spot.hasPlate
                                    ? '修正车牌'
                                    : (spot.plateUnrecognized ? '手动输入车牌' : '录入车牌'),
                                style: const TextStyle(fontWeight: FontWeight.w600),
                              ),
                              style: _outlineActionStyle(),
                            ),
                          ),
                        if (isOccupied && _isCameraPark001(spot))
                          const SizedBox(width: 10),
                        if (_isCameraPark001(spot))
                          Expanded(
                            child: OutlinedButton.icon(
                              // 远程拍照入口: 经【节点】服务 TriggerCapture 触发, 由节点再控制摄像头(间接控制)
                              onPressed:
                                  (provider.capturingRemote || !spot.cameraOnline || freeBlocked)
                                      ? null
                                      : () => _onRemoteCapture(provider),
                              icon: provider.capturingRemote
                                  ? const SizedBox(
                                      width: 16,
                                      height: 16,
                                      child: CircularProgressIndicator(strokeWidth: 2),
                                    )
                                  : (!spot.cameraOnline
                                      ? const Icon(Icons.videocam_off_outlined, size: 18)
                                      : (freeBlocked
                                          ? const Icon(Icons.do_not_disturb_on_outlined, size: 18)
                                          : const Icon(Icons.photo_camera_outlined, size: 18))),
                              label: Text(
                                provider.capturingRemote
                                    ? '拍照识别中...'
                                    : (!spot.cameraOnline
                                        ? '摄像头离线'
                                        : (freeBlocked ? '空闲车位不可拍照' : '远程拍照')),
                                style: const TextStyle(fontWeight: FontWeight.w600),
                                overflow: TextOverflow.ellipsis,
                              ),
                              style: _outlineActionStyle(),
                            ),
                          ),
                      ],
                    ),
                  ],
                ],
              ),
          ),
        ],
      ),
    );
  }

  /// ⭐ 车牌结果层 (三态, 只呈现识别结果本体; 证据组与动作组在卡片下方独立分组):
  ///  - 识别出合法车牌 → PlateBadge (+ 已人工修正角标)
  ///  - '-' 拍到但没认出 → 相机✕图标 + "未识别到车牌" + 靠近/换角度提示
  ///  - 从未上报(null) → 白底"暂无车牌信息"
  /// 说明: 仅车位非空闲(ParkStatus != 0)才可能有值; 车牌是"最近一次识别结果"事件量,
  /// 节点在车走时已清空并上报 '-'(空档期不会残留上一辆车的车牌).
  Widget _buildPlateBadgeArea(SpotModel spot) {
    if (spot.hasPlate) {
      final provider = context.read<ParkingProvider>();
      return Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          PlateBadge(text: spot.plateNumber!, color: spot.plateColor),
          /* 人工修正标记: 提示当前显示的不是节点识别原值 */
          if (provider.hasPlateOverride(spot.id)) ...[
            const SizedBox(height: 4),
            Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.edit_note, size: 13, color: AppColors.primary),
                const SizedBox(width: 3),
                const Text(
                  '已人工修正',
                  style: TextStyle(fontSize: 11, color: AppColors.primary),
                ),
              ],
            ),
          ],
        ],
      );
    }
    if (spot.plateUnrecognized) {
      // '-' : 拍到但没认出 → 引导靠近/换角度重拍 (修正入口在动作组)
      return const Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.no_photography_outlined, size: 42, color: AppColors.textSecondary),
          SizedBox(height: 8),
          Text(
            '未识别到车牌',
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w500,
              color: AppColors.textSecondary,
            ),
          ),
          SizedBox(height: 4),
          Text(
            '请靠近或更换角度后重试',
            style: TextStyle(fontSize: 12, color: AppColors.textSecondary),
          ),
        ],
      );
    }
    // 从未上报 / 空车位 → 白底兜底 (录入入口在动作组)
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

  /// 动作组按钮统一样式: 修正车牌 与 远程拍照 同款整宽描边
  ButtonStyle _outlineActionStyle() => OutlinedButton.styleFrom(
        foregroundColor: AppColors.primary,
        side: BorderSide(color: AppColors.primary.withValues(alpha: 0.5)),
        padding: const EdgeInsets.symmetric(vertical: 12),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
      );

  /// ⭐ 修正车牌: 槽位+三阶段虚拟键盘弹窗 (plate_input_dialog), 校验与输入全在弹窗内完成
  Future<void> _showPlateEditDialog(SpotModel spot) async {
    final provider = context.read<ParkingProvider>();
    final result = await showPlateInputDialog(
      context,
      spot: spot,
      onRestore: provider.hasPlateOverride(spot.id)
          ? () => provider.clearPlateOverride(spot.id)
          : null,
    );
    if (result == null || !mounted) return;
    await provider.setPlateOverride(spot.id, result);
    if (mounted) setState(() {}); // 修正后立即重建 (badge/车牌马上刷新, 不等下轮3s刷新)
  }

  /// 置信度阈值着色: ≥90% 绿 / ≥70% 橙 / <70% 红 (plateConfidence 为 0~100 整数)
  Color _confidenceColor(int conf) {
    if (conf >= 90) return const Color(0xFF16A34A);
    if (conf >= 70) return const Color(0xFFF59E0B);
    return const Color(0xFFEF4444);
  }

  /// 拍照识别信息行 (置信度 / 识别时间)
  Widget _buildCaptureInfoLine(IconData icon, String label, String value, {Color? valueColor}) {
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
            style: TextStyle(
              fontSize: 13,
              color: valueColor ?? AppColors.textPrimary,
              fontWeight: FontWeight.w500,
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
          _buildInfoRow('设备ID', spot.id),
          _buildInfoRow(
              '固件版本',
              _fwVersion ??
                  (_fwLoading ? '读取中…' : '未知')),
          _buildInfoRow('最后更新', _formatUpdatedAt(spot.lastUpdated)),
          // ⭐ 物模型属性区: 默认收起, 点底部箭头展开
          if (_propsExpanded) ...[
            const SizedBox(height: 10),
            const Text(
              '物模型属性',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: AppColors.textSecondary,
              ),
            ),
            if (_ledText(spot) != null) _buildInfoRow('报警灯状态', _ledText(spot)!),
            _buildInfoRow('地磁感应', _geoText(spot.geoMagnetic)),
            _buildInfoRow('超声波距离', '${spot.ultrasonic} cm'),
          ],
          Center(
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: () => setState(() => _propsExpanded = !_propsExpanded),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
                child: Semantics(
                  label: _propsExpanded ? '收起物模型属性' : '展开物模型属性',
                  button: true,
                  child: AnimatedRotation(
                    turns: _propsExpanded ? 0.5 : 0,
                    duration: const Duration(milliseconds: 200),
                    child: const Icon(
                      Icons.expand_more,
                      size: 24,
                      color: AppColors.textSecondary,
                    ),
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// 报警灯状态 (物模型 LED: true/1=触发, false/0=空闲); 未上报 → null 隐藏该行
  String? _ledText(SpotModel spot) {
    final v = spot.rawProperties?['LED'];
    if (v is bool) return v ? '触发' : '空闲';
    if (v is num) return v != 0 ? '触发' : '空闲';
    return null;
  }

  /// 地磁: 0=未感应 1=感应到车 (采样值>1 时直接显示原值)
  String _geoText(int v) => v == 0 ? '未感应' : (v == 1 ? '感应到车' : '$v');

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
            '车位情况',
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
