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
                          _buildSpotLayout(context, provider, spots),
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
          _buildLayoutSwitcher(provider),
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

  Widget _buildLayoutSwitcher(ParkingProvider provider) {
    final mode = provider.layoutMode;
    return GestureDetector(
      onTap: () => provider.setLayoutMode(mode == 0 ? 1 : 0),
      behavior: HitTestBehavior.opaque,
      child: Container(
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: AppColors.surface,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Icon(_layoutIcon(mode), color: AppColors.textSecondary, size: 22),
      ),
    );
  }

  IconData _layoutIcon(int mode) {
    return mode == 0 ? Icons.view_list : Icons.grid_view;
  }

  Widget _buildSpotLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
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

    switch (provider.layoutMode) {
      case 1:
        return _buildGridLayout(context, provider, spots);
      case 0:
      default:
        return _buildListLayout(context, provider, spots);
    }
  }

  /* ==================== 列表布局 ==================== */

  Widget _buildListLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
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
    final bgColor = _getSpotColor(spot);
    final statusText = _getSpotStatusText(spot);
    final iconData = _getSpotIcon(spot);
    final canAct = !spot.isFree && !spot.isOffline;

    return GestureDetector(
      onTap: () => _navigateToDetail(context, spot),
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
                            _getSpotSubText(spot),
                            style: const TextStyle(
                              fontSize: 12,
                              color: AppColors.textSecondary,
                            ),
                          ),
                        ),
                        if (canAct) ...[
                          if (spot.isNotified)
                            const Icon(Icons.notifications_active, size: 14, color: AppColors.primary),
                          if (spot.isNotified && provider.isSpotDispatched(spot.id))
                            const SizedBox(width: 4),
                          if (provider.isSpotDispatched(spot.id))
                            const Icon(Icons.assignment, size: 14, color: AppColors.warning),
                        ],
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  /* ==================== 网格布局 ==================== */

  Widget _buildGridLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    return GridView.builder(
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 2,
        crossAxisSpacing: 10,
        mainAxisSpacing: 10,
        childAspectRatio: 2.0,
      ),
      itemCount: spots.length,
      itemBuilder: (context, index) {
        return _buildSpotGridItem(context, provider, spots[index]);
      },
    );
  }

  Widget _buildSpotGridItem(BuildContext context, ParkingProvider provider, SpotModel spot) {
    final bgColor = _getSpotColor(spot);
    final statusText = _getSpotStatusText(spot);

    return GestureDetector(
      onTap: () => _navigateToDetail(context, spot),
      child: Container(
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.08),
              blurRadius: 8,
              offset: const Offset(0, 2),
            ),
          ],
        ),
        child: Padding(
          padding: const EdgeInsets.all(10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      spot.id,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 14,
                        fontWeight: FontWeight.w700,
                        color: AppColors.textPrimary,
                      ),
                    ),
                  ),
                  const SizedBox(width: 6),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                    decoration: BoxDecoration(
                      color: bgColor.withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(4),
                    ),
                    child: Text(
                      statusText,
                      style: TextStyle(
                        fontSize: 9,
                        fontWeight: FontWeight.w600,
                        color: bgColor,
                      ),
                    ),
                  ),
                ],
              ),
              const Spacer(),
              Text(
                _getSpotSubText(spot),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  fontSize: 11,
                  color: AppColors.textSecondary,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  /* ==================== 辅助方法 ==================== */

  Color _getSpotColor(SpotModel spot) {
    if (spot.isOffline) return AppColors.textSecondary;
    if (spot.isFree) return AppColors.success;
    if (spot.isOccupied) return AppColors.warning;
    return AppColors.danger;
  }

  String _getSpotStatusText(SpotModel spot) {
    if (spot.isOffline) return '离线';
    if (spot.isFree) return '空闲';
    if (spot.isOccupied) return '占用';
    return '僵尸车';
  }

  IconData _getSpotIcon(SpotModel spot) {
    if (spot.isOffline) return Icons.cloud_off;
    if (spot.isFree) return Icons.local_parking;
    if (spot.isOccupied) return Icons.directions_car;
    return Icons.warning_amber;
  }

  String _getSpotSubText(SpotModel spot) {
    if (spot.isOffline) return '设备离线';
    if (spot.isFree) return '暂无车辆';
    return '占用 ${spot.occupiedHours}h';
  }

  void _navigateToDetail(BuildContext context, SpotModel spot) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => SpotDetailPage(spot: spot)),
    );
  }

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
                    MaterialPageRoute(builder: (_) => SpotDetailPage(spot: spot)),
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
}
