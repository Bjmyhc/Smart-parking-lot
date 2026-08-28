class StatsModel {
  final int totalSpots;
  final int occupiedSpots;
  final int zombieSpots;
  final double occupancyRate;
  final List<HourlyTrend> hourlyTrend;

  StatsModel({
    required this.totalSpots,
    required this.occupiedSpots,
    required this.zombieSpots,
    required this.occupancyRate,
    required this.hourlyTrend,
  });

  factory StatsModel.empty() {
    return StatsModel(
      totalSpots: 0,
      occupiedSpots: 0,
      zombieSpots: 0,
      occupancyRate: 0.0,
      hourlyTrend: [],
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
