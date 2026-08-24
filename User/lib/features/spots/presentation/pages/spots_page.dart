import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import 'spot_detail_page.dart';

class SpotsPage extends StatefulWidget {
  const SpotsPage({super.key});

  @override
  State<SpotsPage> createState() => _SpotsPageState();
}

class _SpotsPageState extends State<SpotsPage> {
  bool _batchMode = false;

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final spots = provider.spots;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: provider.isLoading && spots.isEmpty
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              color: AppColors.primary,
              onRefresh: provider.refresh,
              child: SingleChildScrollView(
                physics: const AlwaysScrollableScrollPhysics(),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _buildHeader(context, provider),
                    if (_batchMode) ...[
                      _buildBatchBar(context, provider, spots),
                      const SizedBox(height: 12),
                    ],
                    const SizedBox(height: 16),
                    Padding(
                      padding: const EdgeInsets.fromLTRB(
                        AppDims.paddingPage,
                        0,
                        AppDims.paddingPage,
                        AppDims.paddingPage,
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          _buildSpotGrid(context, provider, spots),
                          const SizedBox(height: 80),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
    );
  }

  Widget _buildHeader(BuildContext context, ParkingProvider provider) {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16,
        left: AppDims.paddingPage,
        right: AppDims.paddingPage,
      ),
      child: Row(
        children: [
          Expanded(
            child: Padding(
              padding: const EdgeInsets.only(left: 8),
              // 隐藏操作: 点击大标题"车位"在 真实模式/本地模式 间切换, 无可见指示
              child: GestureDetector(
                onTap: () => provider.toggleRealMode(),
                behavior: HitTestBehavior.opaque,
                child: const Text(
                  '车位',
                  style: TextStyle(
                    fontSize: 28,
                    fontWeight: FontWeight.w500,
                    color: AppColors.textPrimary,
                  ),
                ),
              ),
            ),
          ),
          _buildHeaderIcon(
            icon: _batchMode ? Icons.close : Icons.checklist,
            onTap: () {
              setState(() {
                _batchMode = !_batchMode;
              });
              if (!_batchMode) {
                provider.clearSpotSelection();
              }
            },
          ),
          const SizedBox(width: 10),
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.search, color: AppColors.textSecondary, size: 22),
          ),
        ],
      ),
    );
  }

  Widget _buildHeaderIcon({required IconData icon, required VoidCallback onTap}) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: _batchMode ? AppColors.primary : AppColors.surface,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Icon(icon, color: _batchMode ? Colors.white : AppColors.textSecondary, size: 22),
      ),
    );
  }

  /// 顶部批量操作栏: [全选] [批量派单] [批量通知], 选中集合收在 Provider.
  Widget _buildBatchBar(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    final selected = provider.selectedSpotIds;
    final allSelected = spots.isNotEmpty && selected.containsAll(spots.map((s) => s.id));
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
                  provider.clearSpotSelection();
                } else {
                  provider.selectAllSpots(spots);
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
                        provider.clearSpotSelection();
                      } else {
                        provider.selectAllSpots(spots);
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
              onTap: () => _onBatchDispatch(context, provider, spots),
            ),
            const SizedBox(width: 8),
            _buildBatchAction(
              label: '批量通知',
              color: AppColors.warning,
              onTap: () => _onBatchNotify(context, provider, spots),
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

  List<SpotModel> _selectedSpots(ParkingProvider provider, List<SpotModel> spots) {
    final selected = provider.selectedSpotIds;
    return spots.where((s) => selected.contains(s.id)).toList();
  }

  Future<void> _onBatchDispatch(BuildContext context, ParkingProvider provider, List<SpotModel> spots) async {
    final targets = _selectedSpots(provider, spots).where((s) => s.isZombie).toList();
    if (targets.isEmpty) {
      _showSnackBar('未选择可派单的僵尸车位');
      return;
    }
    await provider.dispatchSpots(targets);
    provider.clearSpotSelection();
    _showSnackBar('已批量派单 ${targets.length} 台');
  }

  Future<void> _onBatchNotify(BuildContext context, ParkingProvider provider, List<SpotModel> spots) async {
    final targets = _selectedSpots(provider, spots)
        .where((s) => s.isOccupied || s.isZombie)
        .toList();
    if (targets.isEmpty) {
      _showSnackBar('未选择可通知的车位');
      return;
    }
    await provider.notifySpots(targets);
    provider.clearSpotSelection();
    _showSnackBar('已批量通知 ${targets.length} 台');
  }

  Widget _buildSpotGrid(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    if (spots.isEmpty) {
      return Container(
        padding: const EdgeInsets.symmetric(vertical: 60),
        alignment: Alignment.center,
        child: const Column(
          children: [
            Icon(Icons.local_parking, size: 48, color: AppColors.textSecondary),
            SizedBox(height: 12),
            Text(
              '暂无车位',
              style: TextStyle(fontSize: 14, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    return ListView.builder(
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      itemCount: spots.length,
      itemBuilder: (context, index) {
        return _buildSpotListItem(context, provider, spots[index]);
      },
    );
  }

  Widget _buildSpotListItem(BuildContext context, ParkingProvider provider, SpotModel spot) {
    final bgColor = spot.isOffline
        ? AppColors.textSecondary
        : spot.isFree
            ? AppColors.success
            : spot.isOccupied
                ? AppColors.warning
                : AppColors.danger;
    final statusText = spot.isOffline
        ? '离线'
        : spot.isFree
            ? '空闲'
            : spot.isOccupied
                ? '占用'
                : '僵尸车';
    final iconData = spot.isOffline
        ? Icons.cloud_off
        : spot.isFree
            ? Icons.local_parking
            : spot.isOccupied
                ? Icons.directions_car
                : Icons.warning_amber;

    final isSelected = provider.selectedSpotIds.contains(spot.id);
    final hasNotify = spot.isNotified;
    final hasDispatch = provider.isSpotDispatched(spot.id);
    final canAct = !spot.isFree && !spot.isOffline;

    return GestureDetector(
      onTap: () {
        if (_batchMode) {
          if (spot.isOffline) return;
          provider.toggleSpotSelection(spot.id);
        } else {
          Navigator.push(
            context,
            MaterialPageRoute(
              builder: (context) => SpotDetailPage(spot: spot),
            ),
          );
        }
      },
      child: Container(
        margin: const EdgeInsets.only(bottom: 8),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
          border: _batchMode && isSelected
              ? Border.all(color: AppColors.primary, width: 1.5)
              : null,
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
          child: Row(
            children: [
              if (_batchMode) ...[
                Checkbox(
                  value: isSelected,
                  onChanged: spot.isOffline
                      ? null
                      : (_) => provider.toggleSpotSelection(spot.id),
                  activeColor: AppColors.primary,
                  visualDensity: VisualDensity.compact,
                  materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
                const SizedBox(width: 4),
              ],
              Container(
                width: 40,
                height: 40,
                decoration: BoxDecoration(
                  color: bgColor.withValues(alpha: 0.12),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Icon(iconData, color: bgColor, size: 22),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Text(
                          spot.id,
                          style: const TextStyle(
                            fontSize: 15,
                            fontWeight: FontWeight.w700,
                            color: AppColors.textPrimary,
                          ),
                        ),
                        const SizedBox(width: 8),
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                          decoration: BoxDecoration(
                            color: bgColor.withValues(alpha: 0.12),
                            borderRadius: BorderRadius.circular(6),
                          ),
                          child: Text(
                            statusText,
                            style: TextStyle(
                              fontSize: 11,
                              fontWeight: FontWeight.w600,
                              color: bgColor,
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 4),
                    Row(
                      children: [
                        Expanded(
                          child: Text(
                            spot.isOffline
                                ? '设备离线，无法获取状态'
                                : spot.isFree
                                    ? '暂无车辆'
                                    : '占用 ${spot.occupiedHours}h',
                            style: const TextStyle(
                              fontSize: 12,
                              color: AppColors.textSecondary,
                            ),
                          ),
                        ),
                        if (canAct && (hasNotify || hasDispatch)) ...[
                          if (hasNotify)
                            const Icon(Icons.notifications_active, size: 14, color: AppColors.primary),
                          if (hasNotify && hasDispatch) const SizedBox(width: 4),
                          if (hasDispatch)
                            const Icon(Icons.assignment, size: 14, color: AppColors.warning),
                        ],
                      ],
                    ),
                  ],
                ),
              ),
              if (canAct && !_batchMode)
                GestureDetector(
                  onTap: () => _showSpotMenu(context, spot),
                  behavior: HitTestBehavior.opaque,
                  child: Container(
                    width: 32,
                    height: 32,
                    decoration: BoxDecoration(
                      color: AppColors.surface,
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: const Icon(Icons.more_vert, size: 18, color: AppColors.textSecondary),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  /// 三点菜单: 唯一处理入口在详情页, 这里只保留"查看详情".
  void _showSpotMenu(BuildContext context, SpotModel spot) {
    showModalBottomSheet(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (context) {
        return Container(
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
                    Text(
                      spot.id,
                      style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                    ),
                    const Spacer(),
                    GestureDetector(
                      onTap: () => Navigator.pop(context),
                      child: const Icon(Icons.close, size: 22, color: AppColors.textSecondary),
                    ),
                  ],
                ),
              ),
              const Divider(height: 1),
              ListTile(
                leading: const Icon(Icons.info_outline, color: AppColors.primary, size: 22),
                title: const Text(
                  '查看详情',
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                ),
                subtitle: const Text('查看车位、车辆、设备与告警信息', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                onTap: () {
                  Navigator.pop(context);
                  Navigator.push(
                    context,
                    MaterialPageRoute(builder: (context) => SpotDetailPage(spot: spot)),
                  );
                },
              ),
              const SizedBox(height: 8),
            ],
          ),
        );
      },
    );
  }

  void _showSnackBar(String message) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }
}
