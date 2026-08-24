import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../spots/presentation/pages/spot_detail_page.dart';

class AlertsPage extends StatefulWidget {
  const AlertsPage({super.key});

  @override
  State<AlertsPage> createState() => _AlertsPageState();
}

class _AlertsPageState extends State<AlertsPage> {
  int _filterIndex = 0;
  bool _batchMode = false;

  List<AlertModel> _filteredAlerts(List<AlertModel> alerts) {
    switch (_filterIndex) {
      case 1:
        return alerts.where((a) => a.status == 'pending').toList();
      case 2:
        return alerts.where((a) => a.status == 'dispatched').toList();
      case 3:
        return alerts.where((a) => a.status == 'resolved').toList();
      case 4:
        return alerts.where((a) => a.status == 'ignored').toList();
      default:
        return alerts;
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final alerts = provider.allAlerts;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: Column(
        children: [
          _buildHeader(provider),
          _buildFilterTabs(),
          if (_batchMode) ...[
            _buildBatchBar(context, provider, alerts),
            const SizedBox(height: 8),
          ],
          Expanded(
            child: provider.isLoading && alerts.isEmpty
                ? const Center(child: CircularProgressIndicator())
                : _buildBody(context, provider, alerts),
          ),
        ],
      ),
    );
  }

  Widget _buildHeader(ParkingProvider provider) {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16,
        left: AppDims.paddingPage,
        right: AppDims.paddingPage,
        bottom: 16,
      ),
      child: Row(
        children: [
          const Expanded(
            child: Padding(
              padding: EdgeInsets.only(left: 8),
              child: Text(
                '告警管理',
                style: TextStyle(
                  fontSize: 28,
                  fontWeight: FontWeight.w500,
                  color: AppColors.textPrimary,
                ),
              ),
            ),
          ),
          GestureDetector(
            onTap: () {
              setState(() {
                _batchMode = !_batchMode;
              });
              if (!_batchMode) {
                provider.clearAlertSelection();
              }
            },
            child: Container(
              width: 40,
              height: 40,
              decoration: BoxDecoration(
                color: _batchMode ? AppColors.primary : AppColors.surface,
                borderRadius: BorderRadius.circular(12),
              ),
              child: Icon(
                _batchMode ? Icons.close : Icons.checklist,
                color: _batchMode ? Colors.white : AppColors.textSecondary,
                size: 22,
              ),
            ),
          ),
          const SizedBox(width: 10),
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

  Widget _buildFilterTabs() {
    final tabs = ['全部', '待处理', '处理中', '已处理', '已忽略'];
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppDims.paddingPage),
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        child: Row(
          children: List.generate(tabs.length, (i) {
            final isSelected = _filterIndex == i;
            return Container(
              margin: EdgeInsets.only(right: i < tabs.length - 1 ? 10 : 0),
              child: GestureDetector(
                onTap: () {
                  setState(() {
                    _filterIndex = i;
                  });
                },
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
                  decoration: BoxDecoration(
                    color: isSelected ? AppColors.primary : Colors.white,
                    borderRadius: BorderRadius.circular(AppDims.radiusLarge),
                    border: Border.all(
                      color: isSelected ? AppColors.primary : AppColors.textSecondary.withValues(alpha: 0.2),
                      width: 1,
                    ),
                  ),
                  child: Text(
                    tabs[i],
                    style: TextStyle(
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      color: isSelected ? Colors.white : AppColors.textPrimary,
                    ),
                  ),
                ),
              ),
            );
          }),
        ),
      ),
    );
  }

  /// 顶部批量操作栏: [全选] [批量派单] [批量通知], 选中集合收在 Provider.
  Widget _buildBatchBar(BuildContext context, ParkingProvider provider, List<AlertModel> alerts) {
    final list = _filteredAlerts(alerts);
    final selected = provider.selectedAlertIds;
    final allSelected = list.isNotEmpty && selected.containsAll(list.map((a) => a.id));
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppDims.paddingPage),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
          boxShadow: [
            BoxShadow(
              color: AppColors.textSecondary.withValues(alpha: 0.1),
              blurRadius: 6,
              offset: const Offset(0, 2),
            ),
          ],
        ),
        child: Row(
          children: [
            GestureDetector(
              onTap: () {
                if (allSelected) {
                  provider.clearAlertSelection();
                } else {
                  provider.selectAllAlerts(list);
                }
              },
              behavior: HitTestBehavior.opaque,
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Checkbox(
                    value: allSelected,
                    onChanged: (_) {
                      if (allSelected) {
                        provider.clearAlertSelection();
                      } else {
                        provider.selectAllAlerts(list);
                      }
                    },
                    activeColor: AppColors.primary,
                    visualDensity: VisualDensity.compact,
                  ),
                  const Text(
                    '全选',
                    style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                  ),
                ],
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                '已选 ${selected.length} 项',
                style: const TextStyle(fontSize: 13, color: AppColors.textSecondary),
              ),
            ),
            _buildBatchAction(
              label: '批量派单',
              color: AppColors.primary,
              onTap: () => _onBatchDispatch(context, provider, list),
            ),
            const SizedBox(width: 8),
            _buildBatchAction(
              label: '批量通知',
              color: AppColors.warning,
              onTap: () => _onBatchNotify(context, provider, list),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildBatchAction({
    required String label,
    required Color color,
    required VoidCallback onTap,
  }) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.1),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: Text(
          label,
          style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: color),
        ),
      ),
    );
  }

  /// 把选中的告警映射到对应车位 (在 Provider.spots 中按 spotId 查找).
  List<SpotModel> _selectedSpots(ParkingProvider provider, List<AlertModel> alerts) {
    final selected = provider.selectedAlertIds;
    final spotsById = {for (final s in provider.spots) s.id: s};
    final result = <SpotModel>[];
    for (final a in alerts) {
      if (!selected.contains(a.id)) continue;
      final spot = spotsById[a.spotId];
      if (spot != null) result.add(spot);
    }
    return result;
  }

  Future<void> _onBatchDispatch(BuildContext context, ParkingProvider provider, List<AlertModel> list) async {
    final targets = _selectedSpots(provider, list).where((s) => s.isZombie).toList();
    if (targets.isEmpty) {
      _showSnackBar('未选择可派单的僵尸车位告警');
      return;
    }
    await provider.dispatchSpots(targets);
    provider.clearAlertSelection();
    _showSnackBar('已批量派单 ${targets.length} 条');
  }

  Future<void> _onBatchNotify(BuildContext context, ParkingProvider provider, List<AlertModel> list) async {
    final targets = _selectedSpots(provider, list)
        .where((s) => s.isOccupied || s.isZombie)
        .toList();
    if (targets.isEmpty) {
      _showSnackBar('未选择可通知的车位告警');
      return;
    }
    await provider.notifySpots(targets);
    provider.clearAlertSelection();
    _showSnackBar('已批量通知 ${targets.length} 条');
  }

  Widget _buildBody(BuildContext context, ParkingProvider provider, List<AlertModel> alerts) {
    final list = _filteredAlerts(alerts);
    if (list.isEmpty) {
      return const Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(Icons.warning_amber, size: 64, color: AppColors.textSecondary),
            SizedBox(height: 16),
            Text(
              '暂无僵尸车告警',
              style: TextStyle(fontSize: 16, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    return RefreshIndicator(
      color: AppColors.primary,
      onRefresh: provider.refresh,
      child: ListView.builder(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        itemCount: list.length,
        itemBuilder: (context, index) {
          return _buildAlertCard(context, provider, list[index]);
        },
      ),
    );
  }

  Widget _buildAlertCard(BuildContext context, ParkingProvider provider, AlertModel alert) {
    Color iconBgColor;
    Color iconColor;
    IconData iconData;

    switch (alert.status) {
      case 'pending':
        iconBgColor = AppColors.danger.withValues(alpha: 0.1);
        iconColor = AppColors.danger;
        iconData = Icons.warning_amber;
        break;
      case 'dispatched':
        iconBgColor = AppColors.warning.withValues(alpha: 0.1);
        iconColor = AppColors.warning;
        iconData = Icons.assignment;
        break;
      case 'ignored':
        iconBgColor = AppColors.textSecondary.withValues(alpha: 0.1);
        iconColor = AppColors.textSecondary;
        iconData = Icons.not_interested;
        break;
      case 'resolved':
      default:
        iconBgColor = AppColors.success.withValues(alpha: 0.1);
        iconColor = AppColors.success;
        iconData = Icons.check_circle;
        break;
    }

    final isSelected = provider.selectedAlertIds.contains(alert.id);
    final showActions = alert.status == 'pending' || alert.status == 'dispatched';

    return Container(
      margin: const EdgeInsets.only(bottom: AppDims.gapCard),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(AppDims.radiusLarge),
        border: _batchMode && isSelected
            ? Border.all(color: AppColors.primary, width: 1.5)
            : null,
      ),
      child: CardContainer(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (_batchMode) ...[
                  Checkbox(
                    value: isSelected,
                    onChanged: (_) => provider.toggleAlertSelection(alert.id),
                    activeColor: AppColors.primary,
                    visualDensity: VisualDensity.compact,
                    materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  const SizedBox(width: 4),
                ],
                Container(
                  width: 56,
                  height: 56,
                  decoration: BoxDecoration(
                    color: iconBgColor,
                    borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                  ),
                  child: Icon(iconData, color: iconColor, size: 30),
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
                              '僵尸车告警 - ${alert.plateNumber}',
                              style: const TextStyle(
                                fontSize: 16,
                                fontWeight: FontWeight.w700,
                                color: AppColors.textPrimary,
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          StatusBadge.fromStatus(alert.status),
                        ],
                      ),
                      const SizedBox(height: 8),
                      Text(
                        '车位: ${alert.spotId} · 占用 ${alert.occupiedHours}小时',
                        style: const TextStyle(
                          fontSize: 13,
                          color: AppColors.textSecondary,
                        ),
                      ),
                      const SizedBox(height: 4),
                      if (alert.createdAt != null)
                        Text(
                          '创建时间: ${_formatDate(alert.createdAt!)}',
                          style: const TextStyle(
                            fontSize: 12,
                            color: AppColors.textSecondary,
                          ),
                        ),
                    ],
                  ),
                ),
              ],
            ),
            if (showActions) ...[
              const SizedBox(height: 16),
              Row(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  _buildIgnoreButton(context, alert),
                  const SizedBox(width: 12),
                  _buildProcessButton(context, alert),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildIgnoreButton(BuildContext context, AlertModel alert) {
    return GestureDetector(
      onTap: () async {
        final provider = context.read<ParkingProvider>();
        final confirmed = await showModalBottomSheet<bool>(
          context: context,
          backgroundColor: Colors.transparent,
          isScrollControlled: true,
          builder: (ctx) => Container(
            decoration: const BoxDecoration(
              color: Colors.white,
              borderRadius: BorderRadius.vertical(top: Radius.circular(24)),
            ),
            padding: EdgeInsets.only(
              left: 24,
              right: 24,
              top: 20,
              bottom: MediaQuery.of(ctx).padding.bottom + 20,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Container(
                  width: 40,
                  height: 4,
                  decoration: BoxDecoration(
                    color: AppColors.textSecondary.withValues(alpha: 0.2),
                    borderRadius: BorderRadius.circular(2),
                  ),
                ),
                const SizedBox(height: 20),
                const Text(
                  '确认忽略',
                  style: TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.w700,
                    color: AppColors.textPrimary,
                  ),
                ),
                const SizedBox(height: 8),
                const Text(
                  '确定要忽略该告警吗？忽略后将归入"已忽略"，可在筛选标签中查看。',
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    fontSize: 14,
                    color: AppColors.textSecondary,
                    height: 1.5,
                  ),
                ),
                const SizedBox(height: 28),
                GestureDetector(
                  onTap: () => Navigator.pop(ctx, true),
                  child: Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(vertical: 15),
                    decoration: BoxDecoration(
                      color: AppColors.primary,
                      borderRadius: BorderRadius.circular(14),
                    ),
                    child: const Text(
                      '确认忽略',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                        color: Colors.white,
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 12),
                GestureDetector(
                  onTap: () => Navigator.pop(ctx, false),
                  child: Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(vertical: 15),
                    decoration: BoxDecoration(
                      color: AppColors.textSecondary.withValues(alpha: 0.08),
                      borderRadius: BorderRadius.circular(14),
                    ),
                    child: const Text(
                      '取消',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w500,
                        color: AppColors.textSecondary,
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        );
        if (confirmed == true) {
          _showSnackBar('已忽略');
          provider.ignoreAlert(alert);
        }
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.textSecondary.withValues(alpha: 0.08),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: const Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.not_interested, color: AppColors.textSecondary, size: 18),
            SizedBox(width: 6),
            Text(
              '忽略',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: AppColors.textSecondary,
              ),
            ),
          ],
        ),
      ),
    );
  }

  /// 处理按钮: 唯一处理入口在车位详情页, 这里只做跳转.
  Widget _buildProcessButton(BuildContext context, AlertModel alert) {
    return GestureDetector(
      onTap: () => _goToDetail(alert),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.primary,
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: const Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.arrow_forward, color: Colors.white, size: 18),
            SizedBox(width: 6),
            Text(
              '处理',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: Colors.white,
              ),
            ),
          ],
        ),
      ),
    );
  }

  void _goToDetail(AlertModel alert) {
    final spotsById = {for (final s in context.read<ParkingProvider>().spots) s.id: s};
    final spot = spotsById[alert.spotId];
    if (spot == null) {
      _showSnackBar('未找到对应车位');
      return;
    }
    Navigator.push(
      context,
      MaterialPageRoute(builder: (context) => SpotDetailPage(spot: spot)),
    );
  }

  String _formatDate(DateTime time) {
    return '${time.year}-${time.month}-${time.day}';
  }

  void _showSnackBar(String message) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message)),
    );
  }
}
