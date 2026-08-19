import 'package:flutter/material.dart';
import 'package:fl_chart/fl_chart.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/services/api_service.dart';

class OverviewPage extends StatefulWidget {
  const OverviewPage({super.key});

  @override
  State<OverviewPage> createState() => _OverviewPageState();
}

class _OverviewPageState extends State<OverviewPage> {
  final ApiService _apiService = ApiService();
  List<SpotModel> _spots = [];
  List<AlertModel> _alerts = [];
  bool _isLoading = true;

  @override
  void initState() {
    super.initState();
    _loadData();
  }

  Future<void> _loadData() async {
    setState(() {
      _isLoading = true;
    });
    try {
      final spots = await _apiService.getSpots();
      final alerts = await _apiService.getAlerts();
      if (mounted) {
        setState(() {
          _spots = spots;
          _alerts = alerts;
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

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      body: _isLoading
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              color: AppColors.primary,
              onRefresh: _loadData,
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
                            _buildKpiCards(),
                            const SizedBox(height: 16),
                            _buildOccupancyChart(),
                            const SizedBox(height: 16),
                            _buildLatestAlert(),
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
    final hour = DateTime.now().hour;
    final greeting = hour < 6 ? '凌晨好' : hour < 12 ? '早上好' : hour < 18 ? '下午好' : '晚上好';
    final dateStr = '${DateTime.now().month}月${DateTime.now().day}日';

    return Stack(
      clipBehavior: Clip.none,
      children: [
        Container(
          height: 220,
          decoration: const BoxDecoration(
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [Color(0xFF0052D9), Color(0xFF007DFF), Color(0xFF4A9EFF)],
            ),
          ),
        ),
        Positioned(
          top: -30,
          right: -30,
          child: Container(
            width: 150,
            height: 150,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.08),
              shape: BoxShape.circle,
            ),
          ),
        ),
        Positioned(
          top: 40,
          right: 20,
          child: Container(
            width: 80,
            height: 80,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.06),
              shape: BoxShape.circle,
            ),
          ),
        ),
        Positioned(
          top: 80,
          left: -20,
          child: Container(
            width: 100,
            height: 100,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.05),
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
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          '总览',
                          style: TextStyle(
                            fontSize: 28,
                            fontWeight: FontWeight.w700,
                            color: Colors.white,
                          ),
                        ),
                        const SizedBox(height: 4),
                        Text(
                          '$greeting，管理员 · $dateStr',
                          style: const TextStyle(
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
                    child: const Icon(Icons.notifications, color: Colors.white, size: 22),
                  ),
                ],
              ),
              const SizedBox(height: 20),
              _buildHealthRow(),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildHealthRow() {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
      decoration: BoxDecoration(
        color: Colors.white.withOpacity(0.15),
        borderRadius: BorderRadius.circular(AppDims.radiusMedium),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceAround,
        children: [
          _buildHealthItem('LoRa网关', true),
          Container(width: 1, height: 28, color: Colors.white.withOpacity(0.3)),
          _buildHealthItem('摄像头', true),
          Container(width: 1, height: 28, color: Colors.white.withOpacity(0.3)),
          _buildHealthItem('服务器', true),
        ],
      ),
    );
  }

  Widget _buildHealthItem(String label, bool isOnline) {
    return Expanded(
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: isOnline ? AppColors.success : AppColors.danger,
              shape: BoxShape.circle,
            ),
          ),
          const SizedBox(width: 6),
          Text(
            label,
            style: const TextStyle(fontSize: 12, color: Colors.white70),
          ),
        ],
      ),
    );
  }

  Widget _buildKpiCards() {
    final totalSpots = _spots.length;
    final freeSpots = _spots.where((s) => s.isFree).length;
    final occupiedSpots = _spots.where((s) => s.isOccupied).length;
    final zombieSpots = _spots.where((s) => s.isZombie).length;

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
          _buildKpiItem('$totalSpots', '总车位', AppColors.primary),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildKpiItem('$freeSpots', '空闲', AppColors.success),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildKpiItem('$occupiedSpots', '占用', AppColors.warning),
          Container(width: 1, height: 40, color: AppColors.textSecondary.withOpacity(0.15)),
          _buildKpiItem('$zombieSpots', '僵尸车', AppColors.danger),
        ],
      ),
    );
  }

  Widget _buildKpiItem(String value, String label, Color color) {
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

  Widget _buildOccupancyChart() {
    final totalSpots = _spots.length;
    final freeSpots = _spots.where((s) => s.isFree).length;
    final occupiedSpots = _spots.where((s) => s.isOccupied).length;
    final zombieSpots = _spots.where((s) => s.isZombie).length;

    final freeRate = totalSpots > 0 ? (freeSpots / totalSpots) * 100 : 0.0;
    final occupiedRate = totalSpots > 0 ? (occupiedSpots / totalSpots) * 100 : 0.0;
    final zombieRate = totalSpots > 0 ? (zombieSpots / totalSpots) * 100 : 0.0;

    String busiestZone = 'A区';
    double busiestRate = 0;
    for (final zone in ['A', 'B', 'C']) {
      final zoneSpots = _spots.where((s) => s.zone == zone).toList();
      if (zoneSpots.isNotEmpty) {
        final zoneOccupied = zoneSpots.where((s) => s.isOccupied || s.isZombie).length;
        final zoneRate = zoneOccupied / zoneSpots.length;
        if (zoneRate > busiestRate) {
          busiestRate = zoneRate;
          busiestZone = '$zone区';
        }
      }
    }

    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Text(
                '车位实时状态',
                style: TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                  color: AppColors.textPrimary,
                ),
              ),
              const Spacer(),
              Text(
                '共 $totalSpots 个',
                style: const TextStyle(
                  fontSize: 13,
                  color: AppColors.textSecondary,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                flex: 3,
                child: SizedBox(
                  height: 160,
                  child: PieChart(
                    PieChartData(
                      sectionsSpace: 2,
                      centerSpaceRadius: 45,
                      sections: [
                        PieChartSectionData(
                          color: AppColors.success,
                          value: freeRate,
                          title: '${freeRate.toStringAsFixed(0)}%',
                          titleStyle: const TextStyle(
                            fontSize: 11,
                            fontWeight: FontWeight.w600,
                            color: Colors.white,
                          ),
                        ),
                        PieChartSectionData(
                          color: AppColors.warning,
                          value: occupiedRate,
                          title: '${occupiedRate.toStringAsFixed(0)}%',
                          titleStyle: const TextStyle(
                            fontSize: 11,
                            fontWeight: FontWeight.w600,
                            color: Colors.white,
                          ),
                        ),
                        PieChartSectionData(
                          color: AppColors.danger,
                          value: zombieRate,
                          title: zombieRate > 0 ? '${zombieRate.toStringAsFixed(0)}%' : '',
                          titleStyle: const TextStyle(
                            fontSize: 11,
                            fontWeight: FontWeight.w600,
                            color: Colors.white,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
              const SizedBox(width: 16),
              Expanded(
                flex: 2,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _buildChartLegend(AppColors.success, '空闲', freeSpots, freeRate),
                    const SizedBox(height: 12),
                    _buildChartLegend(AppColors.warning, '占用', occupiedSpots, occupiedRate),
                    const SizedBox(height: 12),
                    _buildChartLegend(AppColors.danger, '僵尸车', zombieSpots, zombieRate),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Container(
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(
              color: AppColors.primary.withOpacity(0.05),
              borderRadius: BorderRadius.circular(AppDims.radiusSmall),
            ),
            child: Row(
              children: [
                const Icon(Icons.location_on, size: 14, color: AppColors.primary),
                const SizedBox(width: 4),
                Expanded(
                  child: Text(
                    busiestRate > 0
                        ? '$busiestZone最繁忙，占用率${(busiestRate * 100).toStringAsFixed(0)}%，建议引导车辆至其他区域'
                        : '暂无占用数据',
                    style: const TextStyle(
                      fontSize: 12,
                      color: AppColors.textSecondary,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildChartLegend(Color color, String label, int count, double rate) {
    return Row(
      children: [
        Container(
          width: 10,
          height: 10,
          decoration: BoxDecoration(
            color: color,
            borderRadius: BorderRadius.circular(3),
          ),
        ),
        const SizedBox(width: 6),
        Expanded(
          child: Text(
            label,
            style: const TextStyle(
              fontSize: 12,
              color: AppColors.textSecondary,
            ),
          ),
        ),
        Text(
          '$count个 · ${rate.toStringAsFixed(0)}%',
          style: TextStyle(
            fontSize: 12,
            fontWeight: FontWeight.w600,
            color: color,
          ),
        ),
      ],
    );
  }

  Widget _buildLatestAlert() {
    final latestAlert = _alerts.isNotEmpty ? _alerts.first : null;

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
                  color: AppColors.primary.withOpacity(0.1),
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
          const SizedBox(height: 12),
          if (latestAlert != null)
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: AppColors.danger.withOpacity(0.05),
                borderRadius: BorderRadius.circular(AppDims.radiusMedium),
              ),
              child: Row(
                children: [
                  Container(
                    width: 48,
                    height: 48,
                    decoration: BoxDecoration(
                      color: AppColors.danger.withOpacity(0.1),
                      borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                    ),
                    child: const Icon(Icons.directions_car, color: AppColors.danger, size: 24),
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
