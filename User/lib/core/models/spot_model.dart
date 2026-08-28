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

  /* 是否已在 OneNET 平台停用(enable_status=false):
   * 停用设备同样是"不上线", 但语义与纯粹离线区分(离线可能是网络/断电,
   * 停用是平台主动禁用). 用于 UI 显示"已停用"而非"离线" */
  final bool isDisabled;

  /* 是否真实设备: true=平台真实节点, false=本地模拟车位.
   * 平台下发(服务调用/属性设置)只针对真实设备, 模拟车位不参与下发. */
  final bool isReal;

  /* 传感器原始数据 (OneNET 物模型属性, 用于传感器矛盾诊断) */
  final int geoMagnetic; // 地磁检测值: 0=未感应 / 1=感应到车
  final int ultrasonic;  // 超声波距离(cm)

  /* 本地模拟字段 (处理记录): 仅内存态, 3s 刷新后由 ParkingProvider 回写, 重启即丢 */
  String notifyStatus; // 'none'=未通知 / 'notified'=已通知
  String? handlerName; // 处理人
  DateTime? handledAt; // 完成时间

  SpotModel({
    required this.id,
    this.zone = 'A',
    this.status = 'free',
    this.occupiedHours = 0,
    this.batteryLevel = 100.0,
    this.signalStrength = 0,
    this.geoMagnetic = 0,
    this.ultrasonic = 0,
    this.plateNumber,
    this.lastUpdated,
    this.isOnline = true,
    this.isDisabled = false,
    this.isReal = true,  /* 真实设备默认 true, 模拟车位显式传 false */
    this.notifyStatus = 'none',
    this.handlerName,
    this.handledAt,
  });

  factory SpotModel.fromJson(Map<String, dynamic> json) {
    // 兼容各种来源的 properties: 空 {} 在无类型上下文下可能是 Map<dynamic, dynamic>,
    // 直接 as Map<String, dynamic>? 会抛类型错误, 这里统一转成 Map<String, dynamic>
    final rawProperties = json['properties'];
    final properties = rawProperties is Map
        ? Map<String, dynamic>.from(rawProperties)
        : <String, dynamic>{};
    final isOnline = json['online'] as bool? ?? (json['status'] as String? ?? 'offline') == 'online';
    // 平台停用: enable_status=false 或上游已归一 is_disabled=true
    final isDisabled = json['is_disabled'] == true ||
        json['enable_status'] == false ||
        json['enable_status'] == 'false';

    // 车位状态以节点端上报的 ParkStatus 为准: 0=空闲, 1=有车, 2=僵尸车
    final parkStatus = properties['ParkStatus'] as int? ?? 0;

    String status;
    if (isDisabled) {
      status = 'disabled';
    } else if (!isOnline) {
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

    // 传感器原始数据: 地磁 0/1 + 超声波距离(cm), 用于传感器矛盾诊断
    final geoMagnetic = properties['GeoMagnetic'] as int? ?? 0;
    final ultrasonic = properties['Ultrasonic'] as int? ?? 0;

    final rawName = json['name'] as String? ?? json['deviceName'] as String? ?? '';

    return SpotModel(
      id: rawName,
      zone: _extractZone(rawName),
      status: status,
      occupiedHours: occupiedHours,
      batteryLevel: isOnline ? 85.0 : 0.0,
      signalStrength: isOnline ? -65 : 0,
      geoMagnetic: geoMagnetic,
      ultrasonic: ultrasonic,
      plateNumber: json['plate_number'] as String?,
      lastUpdated: json['updated_at'] as String?,
      isOnline: isOnline,
      isDisabled: isDisabled,
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
      'is_real': isReal,
      'notify_status': notifyStatus,
      'handler_name': handlerName,
      'handled_at': handledAt?.toIso8601String(),
    };
  }

  /// 基于当前车位重建, 仅改动提供参数字段.
  /// 用于本地模拟车位点击图标循环切换状态(内存态, 不落库).
  SpotModel copyWith({
    String? status,
    int? occupiedHours,
    String? plateNumber,
    String? lastUpdated,
    bool? isOnline,
    bool? isDisabled,
    bool? isReal,
    String? notifyStatus,
    String? handlerName,
    DateTime? handledAt,
    double? batteryLevel,
    int? signalStrength,
    int? geoMagnetic,
    int? ultrasonic,
  }) {
    return SpotModel(
      id: id,
      zone: zone,
      status: status ?? this.status,
      occupiedHours: occupiedHours ?? this.occupiedHours,
      batteryLevel: batteryLevel ?? this.batteryLevel,
      signalStrength: signalStrength ?? this.signalStrength,
      geoMagnetic: geoMagnetic ?? this.geoMagnetic,
      ultrasonic: ultrasonic ?? this.ultrasonic,
      plateNumber: plateNumber ?? this.plateNumber,
      lastUpdated: lastUpdated ?? this.lastUpdated,
      isOnline: isOnline ?? this.isOnline,
      isDisabled: isDisabled ?? this.isDisabled,
      isReal: isReal ?? this.isReal,
      notifyStatus: notifyStatus ?? this.notifyStatus,
      handlerName: handlerName ?? this.handlerName,
      handledAt: handledAt ?? this.handledAt,
    );
  }

  bool get isFree => status == 'free';
  bool get isOccupied => status == 'occupied';
  bool get isZombie => status == 'zombie';
  bool get isOffline => status == 'offline';
  bool get isDisabledSpot => status == 'disabled';
  bool get isNotified => notifyStatus == 'notified';
}
