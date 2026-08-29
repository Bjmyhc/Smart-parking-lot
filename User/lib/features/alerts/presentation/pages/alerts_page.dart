import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import 'alert_detail_page.dart';

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
        return alerts.where((a) => a.status == 'notified').toList();
      case 2:
        return alerts.where((a) => a.status == 'dispatched').toList();
      case 3:
        return alerts.where((a) => a.status == 'resolved').toList();
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
    final tabs = ['全部', '已通知', '处理中', '已处理'];
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

  /// 轻量自定义勾选框: 避免 Material Checkbox 在 Web 端触发 ink/circle shader
  /// 编译导致渲染卡死/空白. 仅用 Container + 常规 Icon, 不引入新的渲染路径.
  Widget _buildSelectBox({required bool selected, required VoidCallback onTap}) {
    return GestureDetector(
      onTap: onTap,
      behavior: HitTestBehavior.opaque,
      child: Container(
        width: 22,
        height: 22,
        decoration: BoxDecoration(
          color: selected ? AppColors.primary : Colors.transparent,
          borderRadius: BorderRadius.circular(6),
          border: Border.all(
            color: selected
                ? AppColors.primary
                : AppColors.textSecondary.withValues(alpha: 0.5),
            width: 1.5,
          ),
        ),
        child: selected
            ? const Icon(Icons.check, size: 16, color: Colors.white)
            : null,
      ),
    );
  }

  /// 顶部批量操作栏: [全选] [批量派单] [批量通知], 选中集合收在 Provider.
  /// 全选仅选中当前标签下【可被批量操作】的告警(未通知/已通知), 处理中/已处理不参与.
  Widget _buildBatchBar(BuildContext context, ParkingProvider provider, List<AlertModel> alerts) {
    final list = _filteredAlerts(alerts);
    final actionable = _actionableAlerts(list);
    final selected = provider.selectedAlertIds;
    final allSelected = actionable.isNotEmpty && selected.containsAll(actionable.map((a) => a.id));
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
                  provider.selectAllAlerts(actionable);
                }
              },
              behavior: HitTestBehavior.opaque,
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  _buildSelectBox(
                    selected: allSelected,
                    onTap: () {
                      if (allSelected) {
                        provider.clearAlertSelection();
                      } else {
                        provider.selectAllAlerts(actionable);
                      }
                    },
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

  /// 当前标签下可被批量操作(通知/派单)的告警子集:
  /// 全部=未通知+已通知, 已通知=仅已通知, 处理中/已处理=无.
  List<AlertModel> _actionableAlerts(List<AlertModel> list) {
    final Set<String> statuses;
    if (_filterIndex == 1) {
      statuses = {'notified'};
    } else if (_filterIndex == 2 || _filterIndex == 3) {
      statuses = {};
    } else {
      statuses = {'pending', 'notified'};
    }
    return list.where((a) => statuses.contains(a.status)).toList();
  }

  /// 把【选中且处于指定阶段】的告警映射到对应车位 (在 Provider.spots 中按 spotId 查找).
  /// 批量操作严格按阶段执行: 通知只作用于未通知, 派单只作用于已通知.
  List<SpotModel> _selectedSpotsByStatus(
      ParkingProvider provider, List<AlertModel> alerts, Set<String> statuses) {
    final selected = provider.selectedAlertIds;
    final spotsById = {for (final s in provider.spots) s.id: s};
    final result = <SpotModel>[];
    for (final a in alerts) {
      if (!selected.contains(a.id)) continue;
      if (!statuses.contains(a.status)) continue;
      final spot = spotsById[a.spotId];
      if (spot != null) result.add(spot);
    }
    return result;
  }

  Future<void> _onBatchDispatch(BuildContext context, ParkingProvider provider, List<AlertModel> list) async {
    final targets = _selectedSpotsByStatus(provider, list, {'notified'});
    if (targets.isEmpty) {
      _showSnackBar('未选择可派单的已通知告警');
      return;
    }
    await provider.dispatchSpots(targets);
    provider.clearAlertSelection();
    if (mounted) setState(() => _batchMode = false); // 批量操作完成: 自动退出多选模式
    _showSnackBar('已批量派单 ${targets.length} 条');
  }

  Future<void> _onBatchNotify(BuildContext context, ParkingProvider provider, List<AlertModel> list) async {
    final targets = _selectedSpotsByStatus(provider, list, {'pending'});
    if (targets.isEmpty) {
      _showSnackBar('未选择可通知的未通知告警');
      return;
    }
    await provider.notifySpots(targets);
    provider.clearAlertSelection();
    if (mounted) setState(() => _batchMode = false); // 批量操作完成: 自动退出多选模式
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
      case 'notified':
        iconBgColor = AppColors.primary.withValues(alpha: 0.1);
        iconColor = AppColors.primary;
        iconData = Icons.notifications_active;
        break;
      case 'dispatched':
        iconBgColor = AppColors.warning.withValues(alpha: 0.1);
        iconColor = AppColors.warning;
        iconData = Icons.assignment;
        break;
      case 'resolved':
      default:
        iconBgColor = AppColors.success.withValues(alpha: 0.1);
        iconColor = AppColors.success;
        iconData = Icons.check_circle;
        break;
    }

    final isSelected = provider.selectedAlertIds.contains(alert.id);
    // 所有状态都有对应的操作按钮: pending→通知 / notified→派单 / dispatched或resolved→查看详情
    final showActions = true;

    return GestureDetector(
      /* ⭐ 【所有状态都可以进详情】: 非批量模式下点击卡片任意位置跳转详情页, 彻底消除无入口问题 */
      onTap: _batchMode
          ? null
          : () {
              _goToDetail(alert);
            },
      behavior: HitTestBehavior.opaque,
      child: Container(
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
                    _buildSelectBox(
                      selected: isSelected,
                      onTap: () => provider.toggleAlertSelection(alert.id),
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
                          '车位: ${alert.spotId} · 占用 ${SpotModel.formatOccupiedDuration(alert.occupiedSec)}',
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
                    _buildStatusActionButton(context, provider, alert),
                  ],
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }

  /// 按告警状态自适应的操作按钮组:
  /// - pending:  [查看详情] ── [通知车主]
  /// - notified: [查看详情] ── [派单]
  /// - dispatched / resolved: [查看详情]
  Widget _buildStatusActionButton(
      BuildContext context, ParkingProvider provider, AlertModel alert) {
    switch (alert.status) {
      case 'pending':
        return Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            _buildViewButton(context, alert),
            const SizedBox(width: 8),
            _buildNotifyButton(context, provider, alert),
          ],
        );
      case 'notified':
        return Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            _buildViewButton(context, alert),
            const SizedBox(width: 8),
            _buildDispatchButton(context, provider, alert),
          ],
        );
      case 'dispatched':
      case 'resolved': // 🆕 已处理的告警也可以查看详情(包含完整处理过程时间轴)
      default:
        return _buildViewButton(context, alert);
    }
  }

  /// 通知车主: 标记已通知, 卡片转入"已通知"分类.
  Widget _buildNotifyButton(
      BuildContext context, ParkingProvider provider, AlertModel alert) {
    return GestureDetector(
      onTap: () async {
        final spot = _spotById(alert.spotId);
        if (spot == null) {
          _showSnackBar('未找到对应车位');
          return;
        }
        await provider.notifyOwner(spot);
        _showSnackBar('已通知车主挪车');
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.warning,
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: const Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.notifications_outlined, color: Colors.white, size: 18),
            SizedBox(width: 6),
            Text(
              '通知',
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

  /// 派单: 弹出处理人选择, 派单后卡片转入"处理中"分类.
  Widget _buildDispatchButton(
      BuildContext context, ParkingProvider provider, AlertModel alert) {
    return GestureDetector(
      onTap: () async {
        final spot = _spotById(alert.spotId);
        if (spot == null) {
          _showSnackBar('未找到对应车位');
          return;
        }
        await _showHandlerPicker(provider, spot);
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.primary,
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: const Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.send_outlined, color: Colors.white, size: 18),
            SizedBox(width: 6),
            Text(
              '派单',
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

  /// 查看详情: 处理中/已处理车位仅提供详情入口.
  Widget _buildViewButton(BuildContext context, AlertModel alert) {
    return GestureDetector(
      onTap: () => _goToDetail(alert),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.primary.withValues(alpha: 0.1),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: const Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.arrow_forward, color: AppColors.primary, size: 18),
            SizedBox(width: 6),
            Text(
              '查看详情',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: AppColors.primary,
              ),
            ),
          ],
        ),
      ),
    );
  }

  /// 处理人选择弹窗 (与车位详情页交互一致).
  Future<void> _showHandlerPicker(ParkingProvider provider, SpotModel spot) async {
    const handlers = ['张师傅', '李师傅', '王师傅'];
    final picked = await showModalBottomSheet<String>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) => Container(
        margin: const EdgeInsets.fromLTRB(16, 0, 16, 16),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(16),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 20, 20, 8),
              child: Row(
                children: [
                  const Text(
                    '选择处理人',
                    style: TextStyle(
                      fontSize: 17,
                      fontWeight: FontWeight.w600,
                      color: AppColors.textPrimary,
                    ),
                  ),
                  const Spacer(),
                  GestureDetector(
                    onTap: () => Navigator.pop(ctx),
                    child: const Icon(Icons.close, size: 22, color: AppColors.textSecondary),
                  ),
                ],
              ),
            ),
            const Divider(height: 1),
            ...handlers.map(
              (h) => ListTile(
                leading: const Icon(Icons.person_outline, color: AppColors.primary, size: 22),
                title: Text(
                  h,
                  style: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w600,
                    color: AppColors.textPrimary,
                  ),
                ),
                trailing: const Icon(Icons.chevron_right, size: 20, color: AppColors.textSecondary),
                onTap: () => Navigator.pop(ctx, h),
              ),
            ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
    if (picked != null) {
      await provider.dispatchSpot(spot, handlerName: picked);
      _showSnackBar('已派单给$picked');
    }
  }

  SpotModel? _spotById(String spotId) {
    final spotsById = {for (final s in context.read<ParkingProvider>().spots) s.id: s};
    return spotsById[spotId];
  }

  /// 🆕 查看告警详情: 跳转到【独立的告警工单详情页】
  /// 与车位实时详情解耦 → 即使车位现在没车/设备离线, 也能完整看到:
  /// ✅ 车辆信息(车牌/占用时长/检测时间)
  /// ✅ 完整4步处理时间轴 + 各阶段时间戳/处理人
  /// ✅ 按告警状态显示的操作按钮
  void _goToDetail(AlertModel alert) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (context) => AlertDetailPage(alert: alert)),
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
