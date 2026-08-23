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
  /* 页面自身 UI 状态: 通知/派单记录 (真实/本地模式与数据现统一在 ParkingProvider) */
  final Set<String> _notifySpots = {};
  final Set<String> _dispatchSpots = {};

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
                    _buildHeader(context),
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
                          _buildSpotGrid(spots),
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

  Widget _buildHeader(BuildContext context) {
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
                onTap: () => context.read<ParkingProvider>().toggleRealMode(),
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

  Widget _buildSpotGrid(List<SpotModel> spots) {
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
        return _buildSpotListItem(spots[index]);
      },
    );
  }

  Widget _buildSpotListItem(SpotModel spot) {
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

    final hasNotify = _notifySpots.contains(spot.id);
    final hasDispatch = _dispatchSpots.contains(spot.id);
    final canAct = !spot.isFree && !spot.isOffline;

    return GestureDetector(
      onTap: () {
        Navigator.push(
          context,
          MaterialPageRoute(
            builder: (context) => SpotDetailPage(spot: spot),
          ),
        );
      },
      child: Container(
        margin: const EdgeInsets.only(bottom: 8),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
          child: Row(
            children: [
              Container(
                width: 40,
                height: 40,
                decoration: BoxDecoration(
                  color: bgColor.withOpacity(0.12),
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
                            color: bgColor.withOpacity(0.12),
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
              if (canAct)
                GestureDetector(
                  onTap: () => _showSpotMenu(spot),
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

  void _showSpotMenu(SpotModel spot) {
    showModalBottomSheet(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (context) {
        final hasNotify = _notifySpots.contains(spot.id);
        final hasDispatch = _dispatchSpots.contains(spot.id);
        return Container(
          margin: const EdgeInsets.fromLTRB(16, 0, 16, 16),
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(16),
          ),
          child: Padding(
            padding: const EdgeInsets.fromLTRB(20, 20, 20, 16),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
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
                const SizedBox(height: 16),
                _buildMenuCheckbox(
                  icon: Icons.notifications_outlined,
                  title: '通知车主',
                  subtitle: '通知车主尽快挪走车辆',
                  value: hasNotify,
                  onChanged: (v) {
                    setState(() {
                      if (v == true) {
                        _notifySpots.add(spot.id);
                      } else {
                        _notifySpots.remove(spot.id);
                      }
                    });
                    Navigator.pop(context);
                  },
                ),
                const Divider(height: 1),
                _buildMenuCheckbox(
                  icon: Icons.assignment_outlined,
                  title: '派单',
                  subtitle: '派给挪车师傅处理僵尸车',
                  value: hasDispatch,
                  onChanged: (v) {
                    setState(() {
                      if (v == true) {
                        _dispatchSpots.add(spot.id);
                      } else {
                        _dispatchSpots.remove(spot.id);
                      }
                    });
                    Navigator.pop(context);
                  },
                ),
                const SizedBox(height: 8),
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _buildMenuCheckbox({
    required IconData icon,
    required String title,
    required String subtitle,
    required bool value,
    required ValueChanged<bool?> onChanged,
  }) {
    return GestureDetector(
      onTap: () => onChanged(!value),
      behavior: HitTestBehavior.opaque,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 12),
        child: Row(
          children: [
            Container(
              width: 40,
              height: 40,
              decoration: BoxDecoration(
                color: value ? AppColors.primary.withOpacity(0.12) : AppColors.surface,
                borderRadius: BorderRadius.circular(10),
              ),
              child: Icon(icon, color: value ? AppColors.primary : AppColors.textSecondary, size: 22),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: TextStyle(
                      fontSize: 15,
                      fontWeight: FontWeight.w600,
                      color: value ? AppColors.primary : AppColors.textPrimary,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    subtitle,
                    style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
                  ),
                ],
              ),
            ),
            SizedBox(
              width: 24,
              height: 24,
              child: Checkbox(
                value: value,
                onChanged: onChanged,
                activeColor: AppColors.primary,
                materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                visualDensity: VisualDensity.compact,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
