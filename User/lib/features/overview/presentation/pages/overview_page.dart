import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter/gestures.dart';
import 'dart:async';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../shared/widgets/page_header.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../profile/presentation/pages/diagnosis_page.dart';
import '../../../profile/presentation/pages/firmware_upgrade_page.dart';
import '../../../profile/presentation/pages/policy_config_page.dart';
import '../../../stats/presentation/pages/stats_page.dart';
import '../../../devices/presentation/pages/device_center_page.dart';

/// 首页: 可长按拖动的卡片式首页 (还原自早期提交).
/// 5 张卡片支持长按 1 秒震动后拖动排序, 顺序持久化到本地.
class OverviewPage extends StatefulWidget {
  /// 切换到指定底部 tab (由 MainShell 注入).
  final void Function(int index)? onSwitchTab;

  const OverviewPage({super.key, this.onSwitchTab});

  @override
  State<OverviewPage> createState() => _OverviewPageState();
}

class _OverviewPageState extends State<OverviewPage> {
  static const _allCards = ['greeting', 'quickActions', 'dataCenter', 'device', 'latestAlert'];
  List<String> _cardOrder = List.from(_allCards);

  @override
  void initState() {
    super.initState();
    _loadCardOrder();
  }

  Future<void> _loadCardOrder() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = prefs.getStringList('overview_card_order');
    if (saved != null && saved.length == _allCards.length && saved.toSet().containsAll(_allCards)) {
      setState(() {
        _cardOrder = saved;
      });
    }
  }

  Future<void> _saveCardOrder() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList('overview_card_order', _cardOrder);
  }

  Widget _buildCardById(String id, ParkingProvider provider) {
    switch (id) {
      case 'greeting':
        return _buildGreetingCard();
      case 'quickActions':
        return _buildQuickActions();
      case 'dataCenter':
        return _buildDataCenterCard(provider);
      case 'device':
        return _buildDeviceCenterCard(provider);
      case 'latestAlert':
        return _buildLatestAlertCard(provider.alerts);
      default:
        return _buildGreetingCard();
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    return Scaffold(
      backgroundColor: AppColors.background,
      body: provider.isLoading && provider.spots.isEmpty
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              color: AppColors.primary,
              onRefresh: provider.refresh,
              child: ReorderableListView(
                padding: const EdgeInsets.fromLTRB(
                  AppDims.paddingPage,
                  0,
                  AppDims.paddingPage,
                  80,
                ),
                header: Column(
                  children: [
                    _buildHeader(context, provider),
                    const SizedBox(height: 16),
                  ],
                ),
                buildDefaultDragHandles: false,
                children: _cardOrder.map((id) {
                  return _LongPressDraggable(
                    key: ValueKey(id),
                    index: _cardOrder.indexOf(id),
                    child: _buildCardById(id, provider),
                  );
                }).toList(),
                onReorderItem: (oldIndex, newIndex) {
                  setState(() {
                    final item = _cardOrder.removeAt(oldIndex);
                    _cardOrder.insert(newIndex, item);
                  });
                  _saveCardOrder();
                },
              ),
            ),
    );
  }

  Widget _buildHeader(BuildContext context, ParkingProvider provider) {
    final online = provider.gatewayOnline;
    final badgeColor = online ? AppColors.success : AppColors.danger;
    /* ⭐ 网关状态徽章: 系统中枢, 在线绿/离线红, 离线时醒目提醒 */
    final badge = Container(
      margin: const EdgeInsets.only(right: 8),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: badgeColor.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: badgeColor.withValues(alpha: 0.3), width: 1),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 8, height: 8,
            decoration: BoxDecoration(color: badgeColor, shape: BoxShape.circle),
          ),
          const SizedBox(width: 6),
          Text(
            online ? '网关在线' : '网关离线',
            style: TextStyle(
              fontSize: 12,
              fontWeight: FontWeight.w600,
              color: badgeColor,
            ),
          ),
        ],
      ),
    );
    final bell = GestureDetector(
      onTap: () => widget.onSwitchTab?.call(2),
      child: Container(
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: AppColors.surface,
          borderRadius: BorderRadius.circular(12),
        ),
        child: const Icon(Icons.notifications_none, color: AppColors.textSecondary, size: 22),
      ),
    );
    // 首页在 ReorderableListView 内, 列表已提供左右边距, 故 addHorizontalPadding=false
    return PageHeader(
      title: '首页',
      addHorizontalPadding: false,
      actions: [badge, bell],
    );
  }

  Widget _buildGreetingCard() {
    final hour = DateTime.now().hour;
    final greeting = hour < 6 ? '凌晨好' : hour < 12 ? '早上好' : hour < 18 ? '下午好' : '晚上好';
    final dateStr = '${DateTime.now().month}月${DateTime.now().day}日';
    final weekday = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'][DateTime.now().weekday - 1];

    IconData iconData;
    Color iconColor;
    if (hour < 6) {
      iconData = Icons.bedtime;
      iconColor = AppColors.textSecondary;
    } else if (hour < 12) {
      iconData = Icons.wb_sunny;
      iconColor = const Color(0xFFFFB300);
    } else if (hour < 18) {
      iconData = Icons.wb_cloudy;
      iconColor = AppColors.primary;
    } else {
      iconData = Icons.nightlight;
      iconColor = const Color(0xFF7C4DFF);
    }

    return CardContainer(
      child: Row(
        children: [
          Icon(iconData, color: iconColor, size: 28),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '$greeting，管理员',
                  style: const TextStyle(
                    fontSize: 17,
                    fontWeight: FontWeight.w600,
                    color: AppColors.textPrimary,
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  '$dateStr · $weekday',
                  style: const TextStyle(
                    fontSize: 13,
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

  Widget _buildQuickActions() {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 16),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceAround,
        children: [
          _buildQuickItem(
            Icons.warning_amber,
            '告警中心',
            AppColors.primary,
            () => widget.onSwitchTab?.call(2),
          ),
          _buildQuickItem(
            Icons.tune_rounded,
            '策略配置',
            AppColors.primary,
            () => _push(context, const PolicyConfigPage()),
          ),
          _buildQuickItem(
            Icons.sensors,
            '故障诊断',
            AppColors.primary,
            () => _push(context, const DiagnosisPage()),
          ),
          _buildQuickItem(
            Icons.system_update,
            '固件升级',
            AppColors.primary,
            () => _push(context, const FirmwareUpgradePage()),
          ),
        ],
      ),
    );
  }

  Widget _buildQuickItem(IconData icon, String label, Color color, VoidCallback onTap) {
    return GestureDetector(
      onTap: onTap,
      behavior: HitTestBehavior.opaque,
      child: Column(
        children: [
          Container(
            width: 44,
            height: 44,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Icon(icon, color: color, size: 24),
          ),
          const SizedBox(height: 8),
          Text(
            label,
            style: const TextStyle(
              fontSize: 12,
              color: AppColors.textPrimary,
            ),
          ),
        ],
      ),
    );
  }

  /* ==================== 设备中心卡: 网关状态+节点在线率, 作为设备中心入口 ==================== */
  Widget _buildDeviceCenterCard(ParkingProvider provider) {
    final stats = provider.stats;
    final onlineNodes = stats.onlineDevices - (stats.gatewayOnline ? 1 : 0);
    final totalNodes = stats.totalSpots;
    final rate = totalNodes > 0 ? onlineNodes / totalNodes : 0.0;
    final onlinePct = (rate * 100).round();
    final badgeColor = stats.gatewayOnline ? AppColors.success : AppColors.danger;
    return CardContainer(
      onTap: () => _push(context, const DeviceCenterPage()),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          /* 标题行: 设备中心 + 查看全部 */
          Row(
            children: [
              const Icon(Icons.router, size: 18, color: AppColors.primary),
              const SizedBox(width: 8),
              const Text(
                '设备中心',
                style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
              ),
              const Spacer(),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: AppColors.primary.withValues(alpha: 0.08),
                  borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                ),
                child: const Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text('查看全部', style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: AppColors.primary)),
                    SizedBox(width: 4),
                    Icon(Icons.arrow_forward, size: 14, color: AppColors.primary),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 14),
          /* 网关状态行 */
          Row(
            children: [
              Icon(Icons.wifi_tethering, size: 16, color: badgeColor),
              const SizedBox(width: 6),
              const Text('网关 PGW001', style: TextStyle(fontSize: 13, fontWeight: FontWeight.w500, color: AppColors.textPrimary)),
              const Spacer(),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: badgeColor.withValues(alpha: 0.1),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Container(width: 6, height: 6, decoration: BoxDecoration(color: badgeColor, shape: BoxShape.circle)),
                    const SizedBox(width: 4),
                    Text(stats.gatewayOnline ? '在线' : '离线', style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: badgeColor)),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          /* 节点在线率条 */
          Row(
            children: [
              Text('节点在线 $onlineNodes/$totalNodes', style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
              const SizedBox(width: 8),
              Expanded(
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(3),
                  child: LinearProgressIndicator(
                    value: rate,
                    backgroundColor: AppColors.textSecondary.withValues(alpha: 0.1),
                    valueColor: AlwaysStoppedAnimation(onlineNodes > 0 ? AppColors.success : Colors.grey),
                    minHeight: 6,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              Text('$onlinePct%', style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: AppColors.textSecondary)),
            ],
          ),
        ],
      ),
    );
  }

  /* ==================== 数据中心卡: 合并原车位概览+设备状态, 作为数据页入口 ==================== */
  Widget _buildDataCenterCard(ParkingProvider provider) {
    final stats = provider.stats;
    final occPct = (stats.occupancyRate * 100).round();
    final onlineTotal = stats.totalSpots + 1; // 含网关
    return CardContainer(
      onTap: () => _push(context, const StatsPage()),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          /* 标题行: 数据中心 + 查看全部 */
          Row(
            children: [
              const Icon(Icons.insights, size: 18, color: AppColors.primary),
              const SizedBox(width: 8),
              const Text(
                '数据中心',
                style: TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                  color: AppColors.textPrimary,
                ),
              ),
              const Spacer(),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: AppColors.primary.withValues(alpha: 0.08),
                  borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                ),
                child: const Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      '查看全部',
                      style: TextStyle(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: AppColors.primary,
                      ),
                    ),
                    SizedBox(width: 4),
                    Icon(Icons.arrow_forward, size: 14, color: AppColors.primary),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 14),
          /* KPI 行: 总/空/占/僵尸 (紧凑, 复用数据页同源 stats) */
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceAround,
            children: [
              _buildDcStatItem('${stats.totalSpots}', '总车位', AppColors.textPrimary),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.2)),
              _buildDcStatItem('${stats.freeSpots}', '空闲', AppColors.success),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.2)),
              _buildDcStatItem('${stats.occupiedSpots}', '占用', AppColors.warning),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.2)),
              _buildDcStatItem('${stats.zombieSpots}', '僵尸车', AppColors.danger),
            ],
          ),
          const SizedBox(height: 14),
          /* 占用率条 + 在线设备 */
          Row(
            children: [
              Text(
                '占用率 $occPct%',
                style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(3),
                  child: LinearProgressIndicator(
                    value: stats.occupancyRate,
                    backgroundColor: AppColors.textSecondary.withValues(alpha: 0.1),
                    valueColor: AlwaysStoppedAnimation(
                      stats.occupiedSpots > 0 ? AppColors.warning : AppColors.success,
                    ),
                    minHeight: 6,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Icon(Icons.wifi, size: 14, color: stats.gatewayOnline ? AppColors.success : Colors.grey),
              const SizedBox(width: 4),
              Text(
                '${stats.onlineDevices}/$onlineTotal',
                style: TextStyle(
                  fontSize: 12,
                  fontWeight: FontWeight.w600,
                  color: stats.gatewayOnline ? AppColors.success : Colors.grey,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildDcStatItem(String value, String label, Color color) {
    return Expanded(
      child: Column(
        children: [
          Text(
            value,
            style: TextStyle(
              fontSize: 22,
              fontWeight: FontWeight.w700,
              color: color,
              height: 1,
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
      ),
    );
  }

  Widget _buildLatestAlertCard(List<AlertModel> alerts) {
    final latestAlert = alerts.isNotEmpty ? alerts.first : null;

    return CardContainer(
      onTap: () => widget.onSwitchTab?.call(2),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.warning_amber, size: 18, color: AppColors.danger),
              const SizedBox(width: 8),
              const Text(
                '最新僵尸车告警',
                style: TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                  color: AppColors.textPrimary,
                ),
              ),
              const Spacer(),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: AppColors.textSecondary.withValues(alpha: 0.1),
                  borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                ),
                child: const Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      '查看全部',
                      style: TextStyle(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: AppColors.textSecondary,
                      ),
                    ),
                    SizedBox(width: 4),
                    Icon(Icons.arrow_forward, size: 14, color: AppColors.textSecondary),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          if (latestAlert != null)
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: AppColors.danger.withValues(alpha: 0.05),
                borderRadius: BorderRadius.circular(AppDims.radiusMedium),
              ),
              child: Row(
                children: [
                  Container(
                    width: 44,
                    height: 44,
                    decoration: BoxDecoration(
                      color: AppColors.danger.withValues(alpha: 0.1),
                      borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                    ),
                    child: const Icon(Icons.directions_car, color: AppColors.danger, size: 22),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          latestAlert.plateNumber,
                          style: const TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.w700,
                            color: AppColors.textPrimary,
                          ),
                        ),
                        const SizedBox(height: 4),
                        Text(
                          '车位 ${latestAlert.spotId} · 已占用 ${SpotModel.formatOccupiedDuration(latestAlert.occupiedSec)}',
                          style: const TextStyle(
                            fontSize: 13,
                            color: AppColors.textSecondary,
                          ),
                        ),
                      ],
                    ),
                  ),
                  StatusBadge.fromStatus(latestAlert.status),
                ],
              ),
            )
          else
            Container(
              padding: const EdgeInsets.symmetric(vertical: 20),
              alignment: Alignment.center,
              child: const Text(
                '暂无僵尸车告警',
                style: TextStyle(fontSize: 14, color: AppColors.textSecondary),
              ),
            ),
        ],
      ),
    );
  }

  /// 淡入转场推入子页.
  void _push(BuildContext context, Widget page) {
    Navigator.push(
      context,
      PageRouteBuilder(
        transitionDuration: const Duration(milliseconds: 200),
        pageBuilder: (context, animation, secondaryAnimation) => page,
        transitionsBuilder:
            (context, animation, secondaryAnimation, child) =>
                FadeTransition(opacity: animation, child: child),
      ),
    );
  }
}

/// 长按 1 秒后: 震动 + 进入可拖动状态 (由 _DelayedReorderableDragStartListener 接管).
class _LongPressDraggable extends StatefulWidget {
  final int index;
  final Widget child;

  const _LongPressDraggable({
    super.key,
    required this.index,
    required this.child,
  });

  @override
  State<_LongPressDraggable> createState() => _LongPressDraggableState();
}

class _LongPressDraggableState extends State<_LongPressDraggable> {
  static const _holdDuration = Duration(seconds: 1);
  Timer? _vibrationTimer;

  void _onPointerDown(PointerDownEvent event) {
    _vibrationTimer?.cancel();
    _vibrationTimer = Timer(_holdDuration, () {
      if (mounted) {
        HapticFeedback.mediumImpact();
      }
    });
  }

  void _onPointerUp(PointerUpEvent event) {
    _vibrationTimer?.cancel();
  }

  void _onPointerCancel(PointerCancelEvent event) {
    _vibrationTimer?.cancel();
  }

  @override
  void dispose() {
    _vibrationTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 12),
      child: Listener(
        onPointerDown: _onPointerDown,
        onPointerUp: _onPointerUp,
        onPointerCancel: _onPointerCancel,
        child: _DelayedReorderableDragStartListener(
          index: widget.index,
          delay: _holdDuration,
          child: widget.child,
        ),
      ),
    );
  }
}

class _DelayedReorderableDragStartListener extends ReorderableDragStartListener {
  const _DelayedReorderableDragStartListener({
    required super.child,
    required super.index,
    this.delay = const Duration(seconds: 1),
  });

  final Duration delay;

  @override
  MultiDragGestureRecognizer createRecognizer() {
    return DelayedMultiDragGestureRecognizer(
      delay: delay,
      debugOwner: this,
    );
  }
}
