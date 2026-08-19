import 'package:flutter/material.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/services/api_service.dart';
import 'spot_detail_page.dart';

class SpotsPage extends StatefulWidget {
  const SpotsPage({super.key});

  @override
  State<SpotsPage> createState() => _SpotsPageState();
}

class _SpotsPageState extends State<SpotsPage> {
  final ApiService _apiService = ApiService();
  List<SpotModel> _spots = [];
  List<SpotModel> _filteredSpots = [];
  bool _isLoading = true;
  String _selectedZone = 'A';

  @override
  void initState() {
    super.initState();
    _loadSpots();
  }

  Future<void> _loadSpots() async {
    setState(() {
      _isLoading = true;
    });
    try {
      final spots = await _apiService.getSpots();
      if (mounted) {
        setState(() {
          _spots = spots;
          _filteredSpots = _filterSpots(spots, _selectedZone);
          _isLoading = false;
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _isLoading = false;
        });
      }
    }
  }

  List<SpotModel> _filterSpots(List<SpotModel> spots, String zone) {
    if (zone == 'all') {
      return spots;
    }
    return spots.where((s) => s.zone == zone).toList();
  }

  void _onZoneChanged(String zone) {
    setState(() {
      _selectedZone = zone;
      _filteredSpots = _filterSpots(_spots, zone);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      body: _isLoading
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              color: AppColors.primary,
              onRefresh: _loadSpots,
              child: SingleChildScrollView(
                physics: const AlwaysScrollableScrollPhysics(),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _buildHeader(),
                    Transform.translate(
                      offset: const Offset(0, -30),
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(
                          AppDims.paddingPage,
                          0,
                          AppDims.paddingPage,
                          AppDims.paddingPage,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            _buildStatsSummary(),
                            const SizedBox(height: 16),
                            _buildSpotGrid(),
                            const SizedBox(height: 80),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
    );
  }

  Widget _buildHeader() {
    return Stack(
      clipBehavior: Clip.none,
      children: [
        Container(
          height: 180,
          decoration: const BoxDecoration(
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [Color(0xFF0052D9), Color(0xFF007DFF), Color(0xFF4A9EFF)],
            ),
          ),
        ),
        Positioned(
          top: -20,
          right: -20,
          child: Container(
            width: 120,
            height: 120,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.08),
              shape: BoxShape.circle,
            ),
          ),
        ),
        Positioned(
          top: 50,
          left: -30,
          child: Container(
            width: 80,
            height: 80,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.06),
              shape: BoxShape.circle,
            ),
          ),
        ),
        Padding(
          padding: EdgeInsets.only(
            top: MediaQuery.of(context).padding.top + 16,
            left: AppDims.paddingPage,
            right: AppDims.paddingPage,
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          '车位',
                          style: TextStyle(
                            fontSize: 28,
                            fontWeight: FontWeight.w700,
                            color: Colors.white,
                          ),
                        ),
                        SizedBox(height: 4),
                        Text(
                          '实时监控车位状态',
                          style: TextStyle(
                            fontSize: 14,
                            color: Colors.white70,
                          ),
                        ),
                      ],
                    ),
                  ),
                  Container(
                    width: 40,
                    height: 40,
                    decoration: BoxDecoration(
                      color: Colors.white.withOpacity(0.2),
                      borderRadius: BorderRadius.circular(12),
                    ),
                    child: const Icon(Icons.search, color: Colors.white, size: 22),
                  ),
                ],
              ),
              const SizedBox(height: 16),
              _buildZoneSelector(),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildZoneSelector() {
    return Container(
      padding: const EdgeInsets.all(4),
      decoration: BoxDecoration(
        color: Colors.white.withOpacity(0.15),
        borderRadius: BorderRadius.circular(AppDims.radiusMedium),
      ),
      child: Row(
        children: [
          _buildZoneTab('A区', 'A'),
          _buildZoneTab('B区', 'B'),
          _buildZoneTab('C区', 'C'),
        ],
      ),
    );
  }

  Widget _buildZoneTab(String label, String zone) {
    final isSelected = _selectedZone == zone;
    return Expanded(
      child: GestureDetector(
        onTap: () => _onZoneChanged(zone),
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 10),
          decoration: BoxDecoration(
            color: isSelected ? Colors.white : Colors.transparent,
            borderRadius: BorderRadius.circular(AppDims.radiusSmall),
          ),
          child: Text(
            label,
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: isSelected ? AppColors.primary : Colors.white70,
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildStatsSummary() {
    final total = _filteredSpots.length;
    final free = _filteredSpots.where((s) => s.isFree).length;
    final occupied = _filteredSpots.where((s) => s.isOccupied).length;
    final zombie = _filteredSpots.where((s) => s.isZombie).length;

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(AppDims.radiusLarge),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withOpacity(0.08),
            blurRadius: 16,
            offset: const Offset(0, 4),
          ),
        ],
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceAround,
        children: [
          _buildStatItem('$total', '总车位', AppColors.primary),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildStatItem('$free', '空闲', AppColors.success),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildStatItem('$occupied', '占用', AppColors.warning),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildStatItem('$zombie', '僵尸车', AppColors.danger),
        ],
      ),
    );
  }

  Widget _buildStatItem(String value, String label, Color color) {
    return Expanded(
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
    );
  }

  Widget _buildSpotGrid() {
    if (_filteredSpots.isEmpty) {
      return Container(
        padding: const EdgeInsets.symmetric(vertical: 60),
        alignment: Alignment.center,
        child: const Column(
          children: [
            Icon(Icons.local_parking, size: 48, color: AppColors.textSecondary),
            SizedBox(height: 12),
            Text(
              '该分区暂无车位',
              style: TextStyle(fontSize: 14, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    return GridView.builder(
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 3,
        crossAxisSpacing: 10,
        mainAxisSpacing: 10,
        childAspectRatio: 0.85,
      ),
      itemCount: _filteredSpots.length,
      itemBuilder: (context, index) {
        return _buildSpotGridItem(_filteredSpots[index]);
      },
    );
  }

  Widget _buildSpotGridItem(SpotModel spot) {
    final statusColor = spot.isFree
        ? AppColors.success
        : spot.isOccupied
            ? AppColors.warning
            : AppColors.danger;
    final statusText = spot.isFree
        ? '空闲'
        : spot.isOccupied
            ? '占用'
            : '僵尸车';

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
        decoration: BoxDecoration(
          color: spot.isFree
              ? AppColors.success.withOpacity(0.1)
              : spot.isOccupied
                  ? AppColors.warning.withOpacity(0.1)
                  : AppColors.danger.withOpacity(0.1),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
          border: Border.all(
            color: statusColor.withOpacity(0.3),
            width: 1,
          ),
        ),
        child: Padding(
          padding: const EdgeInsets.all(10),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                decoration: BoxDecoration(
                  color: statusColor.withOpacity(0.2),
                  borderRadius: BorderRadius.circular(4),
                ),
                child: Text(
                  spot.id.length > 6 ? spot.id.substring(spot.id.length - 4) : spot.id,
                  style: TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.w700,
                    color: statusColor,
                  ),
                ),
              ),
              const SizedBox(height: 6),
              Text(
                statusText,
                style: TextStyle(
                  fontSize: 12,
                  fontWeight: FontWeight.w500,
                  color: statusColor,
                ),
              ),
              if (spot.isOccupied || spot.isZombie) ...[
                const SizedBox(height: 4),
                Text(
                  '${spot.occupiedHours}h',
                  style: const TextStyle(
                    fontSize: 11,
                    color: AppColors.textSecondary,
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
