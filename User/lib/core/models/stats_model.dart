class StatsModel {
  final int totalSpots;
  final int occupiedSpots;
  final int zombieSpots;
  final double occupancyRate;
  final List<DailyTrend> weeklyTrend;

  StatsModel({
    required this.totalSpots,
    required this.occupiedSpots,
    required this.zombieSpots,
    required this.occupancyRate,
    required this.weeklyTrend,
  });

  factory StatsModel.fromJson(Map<String, dynamic> json) {
    final weeklyTrendList = json['weekly_trend'] as List? ?? [];
    return StatsModel(
      totalSpots: (json['total_spots'] as num?)?.toInt() ?? 32,
      occupiedSpots: (json['occupied_spots'] as num?)?.toInt() ?? 0,
      zombieSpots: (json['zombie_spots'] as num?)?.toInt() ?? 0,
      occupancyRate: (json['occupancy_rate'] as num?)?.toDouble() ?? 0.0,
      weeklyTrend: weeklyTrendList
          .map((e) => DailyTrend.fromJson(e as Map<String, dynamic>))
          .toList(),
    );
  }

  factory StatsModel.empty() {
    return StatsModel(
      totalSpots: 32,
      occupiedSpots: 0,
      zombieSpots: 0,
      occupancyRate: 0.0,
      weeklyTrend: [],
    );
  }
}

class DailyTrend {
  final String date;
  final int avgOccupancy;
  final int alertsCount;

  DailyTrend({
    required this.date,
    required this.avgOccupancy,
    required this.alertsCount,
  });

  factory DailyTrend.fromJson(Map<String, dynamic> json) {
    return DailyTrend(
      date: json['date'] as String? ?? '',
      avgOccupancy: (json['avg_occupancy'] as num?)?.toInt() ?? 0,
      alertsCount: (json['alerts_count'] as num?)?.toInt() ?? 0,
    );
  }
}
