class SpotModel {
  final String id;
  final String zone;
  final String status;
  final int occupiedHours;
  final double batteryLevel;
  final int signalStrength;
  final String? plateNumber;
  final String? lastUpdated;
  final bool isOnline;

  SpotModel({
    required this.id,
    this.zone = 'A',
    this.status = 'free',
    this.occupiedHours = 0,
    this.batteryLevel = 100.0,
    this.signalStrength = 0,
    this.plateNumber,
    this.lastUpdated,
    this.isOnline = true,
  });

  factory SpotModel.fromJson(Map<String, dynamic> json) {
    // 兼容各种来源的 properties: 空 {} 在无类型上下文下可能是 Map<dynamic, dynamic>,
    // 直接 as Map<String, dynamic>? 会抛类型错误, 这里统一转成 Map<String, dynamic>
    final rawProperties = json['properties'];
    final properties = rawProperties is Map
        ? Map<String, dynamic>.from(rawProperties as Map)
        : <String, dynamic>{};
    final isOnline = json['online'] as bool? ?? (json['status'] as String? ?? 'offline') == 'online';

    // 车位状态以节点端上报的 ParkStatus 为准: 0=空闲, 1=有车, 2=僵尸车
    final parkStatus = properties['ParkStatus'] as int? ?? 0;

    String status;
    if (!isOnline) {
      status = 'offline';
    } else if (parkStatus == 1) {
      status = 'occupied';
    } else if (parkStatus == 2) {
      status = 'zombie';
    } else {
      status = 'free';
    }

    final occupiedTime = properties['OccupiedTime'] as int? ?? 0;
    final occupiedHours = isOnline ? (occupiedTime / 3600).round() : 0;

    final rawName = json['name'] as String? ?? json['deviceName'] as String? ?? '';

    return SpotModel(
      id: rawName,
      zone: _extractZone(rawName),
      status: status,
      occupiedHours: occupiedHours,
      batteryLevel: isOnline ? 85.0 : 0.0,
      signalStrength: isOnline ? -65 : 0,
      plateNumber: json['plate_number'] as String?,
      lastUpdated: json['updated_at'] as String?,
      isOnline: isOnline,
    );
  }

  static String _extractZone(String deviceName) {
    final num = int.tryParse(deviceName) ?? 0;
    if (num <= 3) return 'A';
    if (num <= 6) return 'B';
    return 'C';
  }

  Map<String, dynamic> toJson() {
    return {
      'id': id,
      'zone': zone,
      'status': status,
      'occupied_hours': occupiedHours,
      'battery_level': batteryLevel,
      'signal_strength': signalStrength,
      'plate_number': plateNumber,
      'last_updated': lastUpdated,
      'is_online': isOnline,
    };
  }

  bool get isFree => status == 'free';
  bool get isOccupied => status == 'occupied';
  bool get isZombie => status == 'zombie';
  bool get isOffline => status == 'offline';
}
