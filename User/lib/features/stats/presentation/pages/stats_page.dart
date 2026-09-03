import 'package:flutter/material.dart';
import 'package:fl_chart/fl_chart.dart';
import 'package:provider/provider.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/stats_model.dart';
import '../../../../core/providers/parking_provider.dart';

/// 比赛级数据看板: KPI → 设备状态 → 告警闭环效率 → 趋势图
class StatsPage extends StatelessWidget {
  const StatsPage({super.key});

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final stats = provider.stats;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: SafeArea(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _buildAppBar(context, stats),
            Expanded(
              child: RefreshIndicator(
                color: AppColors.primary,
                onRefresh: provider.refresh,
                child: SingleChildScrollView(
                  physics: const AlwaysScrollableScrollPhysics(),
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(
                      AppDims.paddingPage, 8, AppDims.paddingPage, AppDims.paddingPage,
                    ),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        _buildKpiRow(stats),
                        const SizedBox(height: 14),
                        _buildDeviceStatusCard(stats),
                        const SizedBox(height: 14),
                        _buildEfficiencyCard(stats),
                        const SizedBox(height: 14),
                        _buildTrendCard(stats),
                        const SizedBox(height: 100),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  /* ==================== 顶部栏: 返回 + 标题 + 刷新指示 ==================== */
  Widget _buildAppBar(BuildContext context, StatsModel stats) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Row(
        children: [
          IconButton(
            icon: const Icon(Icons.arrow_back, color: AppColors.textPrimary),
            onPressed: () => Navigator.of(context).pop(),
          ),
          const Text(
            '数据中心',
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
          ),
          const Spacer(),
          // 刷新指示: 网关在线状态 + 最近刷新时间
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            decoration: BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.circular(20),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Container(
                  width: 6, height: 6,
                  decoration: BoxDecoration(
                    color: stats.gatewayOnline ? AppColors.success : Colors.grey,
                    shape: BoxShape.circle,
                  ),
                ),
                const SizedBox(width: 6),
                Text(
                  stats.lastRefreshAgo,
                  style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  /* ==================== KPI 卡片行: 总/空/占/僵尸 ==================== */
  Widget _buildKpiRow(StatsModel stats) {
    return Row(
      children: [
        _buildKpiCard(
          value: stats.totalSpots.toString(),
          label: '总车位',
          color: AppColors.primary,
          flex: 1,
        ),
        const SizedBox(width: 10),
        _buildKpiCard(
          value: stats.freeSpots.toString(),
          label: '空闲',
          color: AppColors.success,
          flex: 1,
        ),
        const SizedBox(width: 10),
        _buildKpiCard(
          value: stats.occupiedSpots.toString(),
          label: '占用',
          color: AppColors.warning,
          flex: 1,
        ),
        const SizedBox(width: 10),
        _buildKpiCard(
          value: stats.zombieSpots.toString(),
          label: '僵尸车',
          color: AppColors.danger,
          flex: 1,
        ),
      ],
    );
  }

  Widget _buildKpiCard({
    required String value, required String label,
    required Color color, required int flex,
  }) {
    return Expanded(
      flex: flex,
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 16, horizontal: 8),
        decoration: BoxDecoration(
          color: AppColors.surface,
          borderRadius: BorderRadius.circular(12),
          boxShadow: [
            BoxShadow(color: Colors.black.withValues(alpha: 0.04), blurRadius: 8, offset: const Offset(0, 2)),
          ],
        ),
        child: Column(
          children: [
            Text(value, style: TextStyle(fontSize: 24, fontWeight: FontWeight.w700, color: color, height: 1)),
            const SizedBox(height: 6),
            Text(label, style: const TextStyle(fontSize: 11, color: AppColors.textSecondary)),
          ],
        ),
      ),
    );
  }

  /* ==================== 设备状态卡: 三段分布环形 + 在线率 ==================== */
  Widget _buildDeviceStatusCard(StatsModel stats) {
    final total = stats.totalSpots;
    final free = stats.freeSpots;
    final occ = stats.occupiedSpots;
    final zom = stats.zombieSpots;
    final used = occ + zom;
    final freePct = total > 0 ? (free * 100 / total) : 0.0;
    final occPct = total > 0 ? (occ * 100 / total) : 0.0;
    final zomPct = total > 0 ? (zom * 100 / total) : 0.0;
    final onlineRate = total > 0 ? stats.onlineDevices / (total + 1) : 0.0;

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [BoxShadow(color: Colors.black.withValues(alpha: 0.04), blurRadius: 8, offset: const Offset(0, 2))],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(width: 4, height: 16, decoration: BoxDecoration(color: AppColors.primary, borderRadius: BorderRadius.circular(2))),
              const SizedBox(width: 8),
              const Text('实时监控', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              SizedBox(
                width: 130, height: 130,
                child: Stack(
                  alignment: Alignment.center,
                  children: [
                    PieChart(
                      PieChartData(
                        sectionsSpace: 1.5,
                        startDegreeOffset: 270,
                        centerSpaceRadius: 36,
                        sections: [
                          if (free > 0) _buildPieSection(freePct, AppColors.success),
                          if (occ > 0) _buildPieSection(occPct, AppColors.warning),
                          if (zom > 0) _buildPieSection(zomPct, AppColors.danger),
                          if (total == 0) PieChartSectionData(
                            value: 100,
                            color: AppColors.textSecondary.withValues(alpha: 0.1),
                            radius: 28,
                            showTitle: false,
                          ),
                        ],
                      ),
                      swapAnimationDuration: const Duration(milliseconds: 500),
                      swapAnimationCurve: Curves.easeOutBack,
                    ),
                    Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Text('$used/$total', style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w700, color: AppColors.textPrimary, height: 1)),
                        const SizedBox(height: 3),
                        const Text('已用/总车位', style: TextStyle(fontSize: 11, color: AppColors.textSecondary)),
                      ],
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _dotLegend(AppColors.success, '空闲', '${freePct.round()}% · $free'),
                    const SizedBox(height: 8),
                    _dotLegend(AppColors.warning, '占用', '${occPct.round()}% · $occ'),
                    const SizedBox(height: 8),
                    _dotLegend(AppColors.danger, '僵尸车', '${zomPct.round()}% · $zom'),
                    const SizedBox(height: 14),
                    Row(
                      children: [
                        Icon(Icons.wifi, size: 14, color: stats.gatewayOnline ? AppColors.success : Colors.grey),
                        const SizedBox(width: 6),
                        const Text('在线设备', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                        const Spacer(),
                        Text('${stats.onlineDevices}', style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: stats.gatewayOnline ? AppColors.success : Colors.grey)),
                        Text('/${total + 1}', style: const TextStyle(fontSize: 11, color: AppColors.textSecondary)),
                      ],
                    ),
                    const SizedBox(height: 6),
                    ClipRRect(
                      borderRadius: BorderRadius.circular(3),
                      child: LinearProgressIndicator(
                        value: onlineRate.clamp(0.0, 1.0),
                        backgroundColor: AppColors.textSecondary.withValues(alpha: 0.1),
                        valueColor: AlwaysStoppedAnimation(stats.gatewayOnline ? AppColors.success : Colors.grey),
                        minHeight: 4,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  PieChartSectionData _buildPieSection(double pct, Color color) {
    final r = (26 + ((pct / 100) * 4).clamp(0, 4)).toDouble();
    return PieChartSectionData(
      value: pct,
      color: color,
      radius: r,
      showTitle: false,
    );
  }

  Widget _dotLegend(Color color, String label, String value) {
    return Row(
      children: [
        Container(width: 10, height: 10, decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(2))),
        const SizedBox(width: 8),
        Expanded(child: Text(label, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary))),
        Text(value, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
      ],
    );
  }

  /* ==================== 告警闭环效率卡: 今日通知/派单/解决 ==================== */
  Widget _buildEfficiencyCard(StatsModel stats) {
    final total = stats.notifiedToday + stats.dispatchedToday + stats.resolvedToday;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [BoxShadow(color: Colors.black.withValues(alpha: 0.04), blurRadius: 8, offset: const Offset(0, 2))],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(width: 4, height: 16, decoration: BoxDecoration(color: AppColors.danger, borderRadius: BorderRadius.circular(2))),
              const SizedBox(width: 8),
              const Text('今日处理效率', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
              const Spacer(),
              Text('共 $total 起', style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              _buildEfficiencyItem(
                count: stats.notifiedToday, label: '已通知',
                icon: Icons.notifications_outlined, color: AppColors.primary,
              ),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.15)),
              _buildEfficiencyItem(
                count: stats.dispatchedToday, label: '已派单',
                icon: Icons.send_outlined, color: AppColors.warning,
              ),
              Container(width: 1, height: 40, color: AppColors.textSecondary.withValues(alpha: 0.15)),
              _buildEfficiencyItem(
                count: stats.resolvedToday, label: '已解决',
                icon: Icons.check_circle_outline, color: AppColors.success,
              ),
            ],
          ),
          if (stats.avgOccupiedMin > 0) ...[
            const SizedBox(height: 14),
            Container(
              padding: const EdgeInsets.all(10),
              decoration: BoxDecoration(
                color: AppColors.primary.withValues(alpha: 0.06),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Row(
                children: [
                  const Icon(Icons.timer, size: 16, color: AppColors.primary),
                  const SizedBox(width: 8),
                  const Text('平均占用时长', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                  const Spacer(),
                  Text(
                    stats.avgOccupiedMin >= 60
                        ? '${stats.avgOccupiedMin ~/ 60}h ${stats.avgOccupiedMin % 60}m'
                        : '${stats.avgOccupiedMin}分钟',
                    style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w700, color: AppColors.primary),
                  ),
                ],
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _buildEfficiencyItem({
    required int count, required String label,
    required IconData icon, required Color color,
  }) {
    return Expanded(
      child: Column(
        children: [
          Container(width: 32, height: 32,
            decoration: BoxDecoration(color: color.withValues(alpha: 0.12), borderRadius: BorderRadius.circular(8)),
            child: Icon(icon, size: 16, color: color),
          ),
          const SizedBox(height: 8),
          Text('$count', style: TextStyle(fontSize: 20, fontWeight: FontWeight.w700, color: color, height: 1)),
          const SizedBox(height: 4),
          Text(label, style: const TextStyle(fontSize: 11, color: AppColors.textSecondary)),
        ],
      ),
    );
  }

  /* ==================== 24小时趋势图 ==================== */
  Widget _buildTrendCard(StatsModel stats) {
    final trend = stats.hourlyTrend;
    final now = DateTime.now();
    final currentHour = now.hour;

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [BoxShadow(color: Colors.black.withValues(alpha: 0.04), blurRadius: 8, offset: const Offset(0, 2))],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(width: 4, height: 16, decoration: BoxDecoration(color: AppColors.primary, borderRadius: BorderRadius.circular(2))),
              const SizedBox(width: 8),
              const Text('24小时占用趋势', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            ],
          ),
          const SizedBox(height: 16),
          SizedBox(
            height: 180,
            child: LineChart(
              LineChartData(
                gridData: FlGridData(
                  show: true,
                  drawVerticalLine: false,
                  horizontalInterval: 25,
                  getDrawingHorizontalLine: (_) => FlLine(color: AppColors.textSecondary.withValues(alpha: 0.15), strokeWidth: 1),
                ),
                titlesData: FlTitlesData(
                  show: true,
                  rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                  topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                  bottomTitles: AxisTitles(
                    sideTitles: SideTitles(
                      showTitles: true,
                      interval: 3,
                      getTitlesWidget: (value, meta) {
                        final h = value.toInt();
                        if (h >= 0 && h < 24 && h % 3 == 0) {
                          return Padding(
                            padding: const EdgeInsets.only(top: 8),
                            child: Text('${h.toString().padLeft(2, '0')}', style: const TextStyle(fontSize: 10, color: AppColors.textSecondary)),
                          );
                        }
                        return const SizedBox.shrink();
                      },
                    ),
                  ),
                  leftTitles: AxisTitles(
                    sideTitles: SideTitles(
                      showTitles: true,
                      interval: 25,
                      getTitlesWidget: (value, meta) => Text('${value.toInt()}%', style: const TextStyle(fontSize: 11, color: AppColors.textSecondary)),
                    ),
                  ),
                ),
                borderData: FlBorderData(show: false),
                minX: 0, maxX: 23, minY: 0, maxY: 100,
                lineTouchData: LineTouchData(
                  touchTooltipData: LineTouchTooltipData(
                    getTooltipItems: (spots) => spots.map((s) {
                      final h = s.x.toInt();
                      return LineTooltipItem(
                        '${h.toString().padLeft(2, '0')}:00\n${s.y.toStringAsFixed(0)}%',
                        const TextStyle(color: Colors.white, fontWeight: FontWeight.w600, fontSize: 12),
                      );
                    }).toList(),
                    tooltipRoundedRadius: 8,
                    tooltipHorizontalAlignment: FLHorizontalAlignment.center,
                  ),
                ),
                lineBarsData: [
                  LineChartBarData(
                    spots: trend.map((t) => FlSpot(t.hour.toDouble(), t.occupancyRate.toDouble())).toList(),
                    isCurved: true,
                    color: AppColors.primary,
                    barWidth: 2,
                    isStrokeCapRound: true,
                    dotData: FlDotData(
                      show: true,
                      getDotPainter: (spot, percent, barData, index) {
                        final isNow = spot.x == currentHour;
                        return FlDotCirclePainter(
                          radius: isNow ? 5 : 2,
                          color: isNow ? AppColors.primary : AppColors.primary.withValues(alpha: 0.5),
                          strokeColor: Colors.white,
                          strokeWidth: isNow ? 2 : 0,
                        );
                      },
                    ),
                    belowBarData: BarAreaData(
                      show: true,
                      gradient: LinearGradient(
                        colors: [AppColors.primary.withValues(alpha: 0.25), AppColors.primary.withValues(alpha: 0.02)],
                        begin: Alignment.topCenter, end: Alignment.bottomCenter,
                      ),
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
