class AlertModel {
  final String id;
  final String plateNumber;
  /* ⭐ 拍照识别附加信息快照 (来自车位的拍照车牌, 供告警小票卡按真实车牌配色显示) */
  final String? plateColor;
  final double? plateConfidence;
  final String spotId;
  final int occupiedHours;
  final int occupiedSec; // 🆕 原始占用秒数(保留秒级精度, 自动适配单位显示)
  final String status;
  final String? imageUrl;
  final DateTime? createdAt;

  AlertModel({
    required this.id,
    required this.plateNumber,
    this.plateColor,
    this.plateConfidence,
    required this.spotId,
    required this.occupiedHours,
    this.occupiedSec = 0, // 🆕 原始占用秒数
    this.status = 'pending',
    this.imageUrl,
    this.createdAt,
  });

  factory AlertModel.fromJson(Map<String, dynamic> json) {
    return AlertModel(
      id: json['id'] as String? ?? '',
      plateNumber: json['plate_number'] as String? ?? '',
      plateColor: json['plate_color'] as String?,
      plateConfidence: (json['plate_confidence'] as num?)?.toDouble(),
      spotId: json['spot_id'] as String? ?? '',
      occupiedHours: (json['occupied_hours'] as num?)?.toInt() ?? 0,
      occupiedSec: (json['occupied_sec'] as num?)?.toInt() ?? (json['occupied_hours'] as num? ?? 0).toInt() * 3600, // 🆕 兼容旧数据: 秒不存在就按小时*3600估算
      status: json['status'] as String? ?? 'pending',
      imageUrl: json['image_url'] as String?,
      createdAt: json['created_at'] != null 
          ? DateTime.parse(json['created_at'] as String) 
          : null,
    );
  }

  Map<String, dynamic> toJson() {
    return {
      'id': id,
      'plate_number': plateNumber,
      'plate_color': plateColor,
      'plate_confidence': plateConfidence,
      'spot_id': spotId,
      'occupied_hours': occupiedHours,
      'occupied_sec': occupiedSec, // 🆕
      'status': status,
      'image_url': imageUrl,
      'created_at': createdAt?.toIso8601String(),
    };
  }

  bool get isPending => status == 'pending';
  bool get isDispatched => status == 'dispatched';
  bool get isResolved => status == 'resolved';

  /// ⭐ 按事件复用工单时重建对象: 不改id/createdAt/spotId这些事件级不变量，只更新状态/秒数/车牌
  AlertModel copyWith({
    String? plateNumber,
    String? plateColor,
    double? plateConfidence,
    int? occupiedHours,
    int? occupiedSec,
    String? status,
    String? imageUrl,
    DateTime? createdAt,
  }) {
    return AlertModel(
      id: id, /* ⭐ 绝对不能改: 同一个事件保持相同工单id, 新事件才会生成新id */
      spotId: spotId, /* ⭐ 同一个车位 */
      plateNumber: plateNumber ?? this.plateNumber,
      plateColor: plateColor ?? this.plateColor,
      plateConfidence: plateConfidence ?? this.plateConfidence,
      occupiedHours: occupiedHours ?? this.occupiedHours,
      occupiedSec: occupiedSec ?? this.occupiedSec,
      status: status ?? this.status,
      imageUrl: imageUrl ?? this.imageUrl,
      createdAt: createdAt ?? this.createdAt,
    );
  }
}
