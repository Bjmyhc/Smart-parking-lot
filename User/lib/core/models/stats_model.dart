class StatsModel {
  final int totalSpots;
  final int occupiedSpots;
  final int zombieSpots;
  final double occupancyRate;
  final List<HourlyTrend> hourlyTrend;

  /* ⭐ 比赛级指标: 证明系统真的在工作 */
  final int freeSpots;            // 空闲数
  final int onlineDevices;        // 在线设备数(含网关)
  final bool gatewayOnline;       // 网关在线
  final int notifiedToday;        // 今日已通知车主数
  final int dispatchedToday;      // 今日已派单数
  final int resolvedToday;        // 今日已解决数
  final int avgOccupiedMin;       // 平均占用时长(分钟)
  final String lastRefreshAgo;    // 上次刷新距今 "3秒前"

  StatsModel({
    required this.totalSpots,
    required this.occupiedSpots,
    required this.zombieSpots,
    required this.occupancyRate,
    required this.hourlyTrend,
    required this.freeSpots,
    required this.onlineDevices,
    required this.gatewayOnline,
    required this.notifiedToday,
    required this.dispatchedToday,
    required this.resolvedToday,
    required this.avgOccupiedMin,
    required this.lastRefreshAgo,
  });

  factory StatsModel.empty() {
    return StatsModel(
      totalSpots: 0,
      occupiedSpots: 0,
      zombieSpots: 0,
      occupancyRate: 0.0,
      hourlyTrend: [],
      freeSpots: 0,
      onlineDevices: 0,
      gatewayOnline: false,
      notifiedToday: 0,
      dispatchedToday: 0,
      resolvedToday: 0,
      avgOccupiedMin: 0,
      lastRefreshAgo: '-',
    );
  }
}

class HourlyTrend {
  final int hour;
  final int occupancyRate;

  HourlyTrend({
    required this.hour,
    required this.occupancyRate,
  });
}
