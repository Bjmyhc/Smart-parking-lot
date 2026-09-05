class SpotModel {
  final String id;
  final String zone;
  final String status;
  final int occupiedHours;
  final int occupiedSec; // 🆕 原始占用秒数(节点上报值, 仅作为首轮拉取时的兜底)
  /* ⭐ 进入占用状态的【本地时间戳】: 用于实时计算占用时间, 不再依赖节点定时上报的OccupiedTime属性 */
  DateTime? occupiedSince;
  final double batteryLevel;
  final int signalStrength;
  final String? plateNumber;
  /* ⭐⭐⭐ 拍照识别附加信息 (车牌只由拍照成功驱动, 摄像头Cam001属性回传):
   * plateColor       = 车牌颜色 (中文: 蓝色/黄色/绿色/白色/黑色/红色/灰色)
   * plateConfidence  = 识别置信度 0~1
   * capturedAt       = 识别时间 (摄像头 CaptureTime)
   * captureFailed    = 本占用事件内已尝试拍照但未识别出车牌 (区别于从未拍照) */
  final String? plateColor;
  final double? plateConfidence;
  final String? capturedAt;
  final bool captureFailed;
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
  final int? zombieThresholdSec; // 🆕 节点端真实僵尸判定阈值(秒), null=未知(离线/未上报)
  final int? sensorDistanceCm;  // 🆕 节点端真实超声波距离阈值(cm), null=未知(离线/未上报)

  /* 本地模拟字段 (处理记录): 仅内存态, 3s 刷新后由 ParkingProvider 回写, 重启即丢 */
  String notifyStatus; // 'none'=未通知 / 'notified'=已通知
  String? handlerName; // 处理人
  DateTime? handledAt; // 完成时间
  DateTime? alertCreatedAt; // 🆕 告警创建时间
  DateTime? notifiedAt;     // 🆕 车主通知时间
  DateTime? dispatchedAt;   // 🆕 派单时间

  SpotModel({
    required this.id,
    this.zone = 'A',
    this.status = 'free',
    this.occupiedHours = 0,
    this.occupiedSec = 0,
    this.occupiedSince, // ⭐ 进入占用状态的本地时间戳 (非final, refresh时可修改)
    this.batteryLevel = 100.0,
    this.signalStrength = 0,
    this.geoMagnetic = 0,
    this.ultrasonic = 0,
    this.zombieThresholdSec,
    this.sensorDistanceCm,
    this.plateNumber,
    this.plateColor,
    this.plateConfidence,
    this.capturedAt,
    this.captureFailed = false,
    this.lastUpdated,
    this.isOnline = true,
    this.isDisabled = false,
    this.isReal = true,
    this.notifyStatus = 'none',
    this.handlerName,
    this.handledAt,
    this.alertCreatedAt,
    this.notifiedAt,
    this.dispatchedAt,
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
    // 🆕 节点端僵尸判定阈值属性(OneNET物模型真实identifier=ZombieThresholdSec, 其余为fallback兼容)
    int? zombieThresholdSec = properties['ZombieThresholdSec'] as int?
        ?? properties['ZombieThreshold'] as int?
        ?? properties['ThresholdValue'] as int?
        ?? properties['threshold_value'] as int?;
    // 🆕 节点端超声波距离阈值属性
    int? sensorDistanceCm = properties['SensorDistanceCm'] as int?;
    // ⭐ 节点信号强度(OneNET物模型真实identifier=SignalRssi, 网关代上报dBm)
    // 读取失败/离线 → null, UI 侧显示占位, 不再用硬编码假值
    final rawRssi = properties['SignalRssi'];
    int? signalStrength;
    if (rawRssi is int) {
      signalStrength = rawRssi;         // 平台返回数值(可能已是int/字符串解析后的int)
    } else if (rawRssi is String && rawRssi.trim().isNotEmpty) {
      signalStrength = int.tryParse(rawRssi); // 兼容字符串形式 "-84"
    }
    final rawName = json['name'] as String? ?? json['deviceName'] as String? ?? '';
    // 调试输出: 真实在线节点阈值未读到 → 打印所有属性key用于排查OneNET真实标识符
    if (isOnline && !isDisabled && zombieThresholdSec == null && rawName.startsWith(RegExp(r'Park|park'))) {
      print('[SpotModel 调试] ⚠️ 节点 $rawName 未读到僵尸阈值属性, 平台属性keys=${properties.keys.toList()}');
      print('[SpotModel 调试]    完整属性values=$properties');
    }

    return SpotModel(
      id: rawName,
      zone: _extractZone(rawName),
      status: status,
      occupiedHours: occupiedHours,
      occupiedSec: isOnline ? occupiedTime : 0, // 🆕 原始占用秒数, 离线=0
      batteryLevel: isOnline ? 100.0 : 0.0, // 电量: 节点接电源无真实采集, 固定显示100%
      signalStrength: isOnline && signalStrength != null ? signalStrength : 0, // ⭐ 读信号强度(网关DRSSI上报)
      geoMagnetic: geoMagnetic,
      ultrasonic: ultrasonic,
      zombieThresholdSec: zombieThresholdSec, // 🆕 节点端上报的真实阈值
      sensorDistanceCm: sensorDistanceCm,   // 🆕 节点端上报的真实超声波距离阈值
      plateNumber: json['plate_number'] as String?,
      plateColor: json['plate_color'] as String?,
      plateConfidence: (json['plate_confidence'] as num?)?.toDouble(),
      capturedAt: json['captured_at'] as String?,
      captureFailed: json['capture_failed'] == true,
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
      'plate_color': plateColor,
      'plate_confidence': plateConfidence,
      'captured_at': capturedAt,
      'capture_failed': captureFailed,
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
    int? occupiedSec,
    DateTime? occupiedSince, // ⭐ 进入占用状态的本地时间戳
    String? plateNumber,
    String? plateColor,
    double? plateConfidence,
    String? capturedAt,
    bool? captureFailed,
    String? lastUpdated,
    bool? isOnline,
    bool? isDisabled,
    bool? isReal,
    String? notifyStatus,
    String? handlerName,
    DateTime? handledAt,
    DateTime? alertCreatedAt,
    DateTime? notifiedAt,
    DateTime? dispatchedAt,
    double? batteryLevel,
    int? signalStrength,
    int? geoMagnetic,
    int? ultrasonic,
    int? zombieThresholdSec,
    int? sensorDistanceCm,
  }) {
    return SpotModel(
      id: id,
      zone: zone,
      status: status ?? this.status,
      occupiedHours: occupiedHours ?? this.occupiedHours,
      occupiedSec: occupiedSec ?? this.occupiedSec,
      occupiedSince: occupiedSince ?? this.occupiedSince, // ⭐
      batteryLevel: batteryLevel ?? this.batteryLevel,
      signalStrength: signalStrength ?? this.signalStrength,
      geoMagnetic: geoMagnetic ?? this.geoMagnetic,
      ultrasonic: ultrasonic ?? this.ultrasonic,
      zombieThresholdSec: zombieThresholdSec ?? this.zombieThresholdSec,
      sensorDistanceCm: sensorDistanceCm ?? this.sensorDistanceCm,
      plateNumber: plateNumber ?? this.plateNumber,
      plateColor: plateColor ?? this.plateColor,
      plateConfidence: plateConfidence ?? this.plateConfidence,
      capturedAt: capturedAt ?? this.capturedAt,
      captureFailed: captureFailed ?? this.captureFailed,
      lastUpdated: lastUpdated ?? this.lastUpdated,
      isOnline: isOnline ?? this.isOnline,
      isDisabled: isDisabled ?? this.isDisabled,
      isReal: isReal ?? this.isReal,
      notifyStatus: notifyStatus ?? this.notifyStatus,
      handlerName: handlerName ?? this.handlerName,
      handledAt: handledAt ?? this.handledAt,
      alertCreatedAt: alertCreatedAt ?? this.alertCreatedAt,
      notifiedAt: notifiedAt ?? this.notifiedAt,
      dispatchedAt: dispatchedAt ?? this.dispatchedAt,
    );
  }

  bool get isFree => status == 'free';
  bool get isOccupied => status == 'occupied';
  bool get isZombie => status == 'zombie';
  bool get isOffline => status == 'offline';
  bool get isDisabledSpot => status == 'disabled';
  bool get isNotified => notifyStatus == 'notified';

  /* ⭐⭐⭐ 【实时占用秒数】: 彻底解决刷新滞后问题
   * 原 occupiedSec 完全依赖节点上报的 OccupiedTime 属性, 节点仅在 LoRa 轮询时才更新 → 严重滞后
   * 现在改为用本地时间戳 occupiedSince 动态计算:
   * 1. occupied/zombie 状态: now - occupiedSince 实时秒数
   * 2. free/offline/disabled → 返回0
   * 3. 🆕 懒汉式兜底: 若 occupiedSince 为null (因任何原因未在_updateOccupiedSince设置), 
   *    则首次调用 actualOccupiedSec 时立刻用 occupiedSec 反推一个起始时间戳, 之后秒数就会实时增长
   * 4. 这个 getter 在每次 build 的时候都会重新计算, 手动刷新/轮询刷新都会立刻看到最新占用时间 */
  int get actualOccupiedSec {
    if (isOccupied || isZombie) {
      if (occupiedSince != null) {
        return DateTime.now().difference(occupiedSince!).inSeconds;
      }
      // 🆕 懒汉式兜底: 首次调用时反推并记录, 后面秒数就会实时增长
      final now = DateTime.now();
      if (occupiedSec > 0) {
        occupiedSince = now.subtract(Duration(seconds: occupiedSec));
      } else {
        occupiedSince = now;
      }
      return DateTime.now().difference(occupiedSince!).inSeconds;
    }
    return 0;
  }

  /// 🆕 通用时长格式化: 自动适配秒/分/时单位
  /// 比赛场景下阈值通常设为几十秒/几分钟, 不再显示"占用0小时"
  /// 示例: 50秒→"50秒", 70秒→"1分10秒", 3600秒→"1小时", 3661秒→"1小时1分"
  static String formatOccupiedDuration(int totalSec) {
    if (totalSec <= 0) return '0秒';
    if (totalSec < 60) return '${totalSec}秒';

    final int min = totalSec ~/ 60;
    final int sec = totalSec % 60;
    if (min < 60) {
      return sec == 0 ? '${min}分钟' : '${min}分${sec}秒';
    }

    final int hour = min ~/ 60;
    final int m = min % 60;
    return m == 0 ? '${hour}小时' : '${hour}小时${m}分';
  }
}
