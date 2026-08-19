class SpotModel {
  final String id;
  final String zone;
  final String status;
  final int occupiedHours;
  final double batteryLevel;
  final int signalStrength;
  final String? plateNumber;
  final String? lastUpdated;

  SpotModel({
    required this.id,
    this.zone = 'A',
    this.status = 'free',
    this.occupiedHours = 0,
    this.batteryLevel = 100.0,
    this.signalStrength = 0,
    this.plateNumber,
    this.lastUpdated,
  });

  factory SpotModel.fromJson(Map<String, dynamic> json) {
    final properties = json['properties'] as Map<String, dynamic>? ?? {};
    
    final parkStatus = properties['ParkStatus'] as int? ?? 0;
    final ultrasonic = properties['Ultrasonic'] as int? ?? 350;
    
    String status;
    if (parkStatus == 1 || ultrasonic < 30) {
      status = 'occupied';
    } else {
      status = 'free';
    }

    final occupiedTime = properties['OccupiedTime'] as int? ?? 0;
    final occupiedHours = (occupiedTime / 3600).round();

    return SpotModel(
      id: json['name'] as String? ?? json['deviceName'] as String? ?? '',
      zone: _extractZone(json['name'] as String? ?? ''),
      status: status,
      occupiedHours: occupiedHours,
      batteryLevel: 85.0,
      signalStrength: -65,
      plateNumber: json['plate_number'] as String?,
      lastUpdated: json['updated_at'] as String?,
    );
  }

  static String _extractZone(String deviceName) {
    if (deviceName.startsWith('Park001')) return 'A';
    if (deviceName.startsWith('Park002')) return 'A';
    if (deviceName.startsWith('Park003')) return 'B';
    if (deviceName.startsWith('Park004')) return 'B';
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
    };
  }

  bool get isFree => status == 'free';
  bool get isOccupied => status == 'occupied';
  bool get isZombie => status == 'zombie';
}
