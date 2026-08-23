import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter/gestures.dart';
import 'dart:async';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/providers/parking_provider.dart';

class OverviewPage extends StatefulWidget {
  const OverviewPage({super.key});

  @override
  State<OverviewPage> createState() => _OverviewPageState();
}

class _OverviewPageState extends State<OverviewPage> {
  static const _allCards = ['greeting', 'quickActions', 'deviceStatus', 'stats', 'latestAlert'];
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

  Widget _buildCardById(String id, List<SpotModel> spots, List<AlertModel> alerts) {
    switch (id) {
      case 'greeting':
        return _buildGreetingCard();
      case 'quickActions':
        return _buildQuickActions();
      case 'deviceStatus':
        return _buildDeviceStatusCard(spots);
      case 'stats':
        return _buildStatsCard(spots);
      case 'latestAlert':
        return _buildLatestAlertCard(alerts);
      default:
        return _buildGreetingCard();
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final spots = provider.spots;
    final alerts = provider.alerts;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: provider.isLoading && spots.isEmpty
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
                    _buildHeader(context),
                    const SizedBox(height: 16),
                  ],
                ),
                buildDefaultDragHandles: false,
                children: _cardOrder.map((id) {
                  return _LongPressDraggable(
                    key: ValueKey(id),
                    index: _cardOrder.indexOf(id),
                    child: _buildCardById(id, spots, alerts),
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

  Widget _buildHeader(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16,
        left: AppDims.paddingPage,
        right: AppDims.paddingPage,
      ),
      child: Row(
        children: [
          const Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '首页',
                  style: TextStyle(
                    fontSize: 28,
                    fontWeight: FontWeight.w500,
                    color: AppColors.textPrimary,
                  ),
                ),
              ],
            ),
          ),
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.notifications_none, color: AppColors.textSecondary, size: 22),
          ),
        ],
      ),
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
          _buildQuickItem(Icons.warning_amber, '告警中心', AppColors.primary),
          _buildQuickItem(Icons.local_parking, '车位管理', AppColors.primary),
          _buildQuickItem(Icons.file_download, '数据导出', AppColors.primary),
          _buildQuickItem(Icons.system_update, '系统状态', AppColors.primary),
        ],
      ),
    );
  }

  Widget _buildQuickItem(IconData icon, String label, Color color) {
    return GestureDetector(
      onTap: () {},
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

  Widget _buildStatsCard(List<SpotModel> spots) {
    final totalSpots = spots.length;
    final freeSpots = spots.where((s) => s.isFree).length;
    final occupiedSpots = spots.where((s) => s.isOccupied).length;
    final zombieSpots = spots.where((s) => s.isZombie).length;

    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Padding(
            padding: EdgeInsets.only(bottom: 12),
            child: Text(
              '车位概览',
              style: TextStyle(
                fontSize: 15,
                fontWeight: FontWeight.w600,
                color: AppColors.textPrimary,
              ),
            ),
          ),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceAround,
            children: [
              _buildStatItem('$totalSpots', '总车位', AppColors.textPrimary),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.3)),
              _buildStatItem('$freeSpots', '空闲', AppColors.success),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.3)),
              _buildStatItem('$occupiedSpots', '占用', AppColors.warning),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.3)),
              _buildStatItem('$zombieSpots', '僵尸车', AppColors.danger),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildStatItem(String value, String label, Color color) {
    return Expanded(
      child: GestureDetector(
        onTap: () {
          if (label == '僵尸车') {
            Navigator.pushNamed(context, '/alerts');
          }
        },
        child: Column(
          children: [
            Text(
              value,
              style: TextStyle(
                fontSize: 22,
                fontWeight: FontWeight.w700,
                color: color,
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
      ),
    );
  }

  Widget _buildDeviceStatusCard(List<SpotModel> spots) {
    final total = spots.length;
    final normal = spots.where((s) => s.isFree).length;
    final abnormal = total - normal;

    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.sensors, size: 18, color: AppColors.primary),
              const SizedBox(width: 8),
              const Text(
                '设备状态',
                style: TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                  color: AppColors.textPrimary,
                ),
              ),
              const Spacer(),
              Text(
                '共 $total 个节点',
                style: const TextStyle(
                  fontSize: 12,
                  color: AppColors.textSecondary,
                ),
              ),
            ],
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                child: Container(
                  padding: const EdgeInsets.symmetric(vertical: 10),
                  decoration: BoxDecoration(
                    color: AppColors.success.withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Column(
                    children: [
                      Text(
                        '$normal',
                        style: const TextStyle(
                          fontSize: 22,
                          fontWeight: FontWeight.w700,
                          color: AppColors.success,
                        ),
                      ),
                      const SizedBox(height: 2),
                      const Text(
                        '正常',
                        style: TextStyle(fontSize: 12, color: AppColors.textSecondary),
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: Container(
                  padding: const EdgeInsets.symmetric(vertical: 10),
                  decoration: BoxDecoration(
                    color: abnormal > 0
                        ? AppColors.warning.withValues(alpha: 0.08)
                        : AppColors.textSecondary.withValues(alpha: 0.05),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Column(
                    children: [
                      Text(
                        '$abnormal',
                        style: TextStyle(
                          fontSize: 22,
                          fontWeight: FontWeight.w700,
                          color: abnormal > 0
                              ? AppColors.warning
                              : AppColors.textSecondary,
                        ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        abnormal > 0 ? '异常' : '异常',
                        style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildLatestAlertCard(List<AlertModel> alerts) {
    final latestAlert = alerts.isNotEmpty ? alerts.first : null;

    return CardContainer(
      onTap: () {
        Navigator.pushNamed(context, '/alerts');
      },
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
                          '车位 ${latestAlert.spotId} · 已占用 ${latestAlert.occupiedHours}小时',
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
}

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
