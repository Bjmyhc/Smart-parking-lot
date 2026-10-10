class AlertModel {
  final String id;
  final String plateNumber;
  /* ⭐ 拍照识别附加信息快照 (来自车位节点属性, 供告警小票卡显示)
   * plateConfidence = 识别置信度 int 0~100 */
  final String? plateColor;
  final int? plateConfidence;
  /* ⭐ S5 车牌原图快照 (94×24 点阵的 base64, 工单生成瞬间从车位拷贝).
   * 平台 PlateThumb 是滚动覆盖的最新图, 归档后再取就是别的车 → 必须随工单落盘. */
  final String? plateThumb;
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
    this.plateThumb,
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
      plateConfidence: (json['plate_confidence'] as num?)?.toInt(),
      plateThumb: json['plate_thumb'] as String?,
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
      'plate_thumb': plateThumb, // ⭐ 图随工单快照持久化, 归档后仍可回看"当时"
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
    int? plateConfidence,
    String? plateThumb,
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
      plateThumb: plateThumb ?? this.plateThumb, /* 只补不清: 车位临时无图不抹掉已记录的图 */
      occupiedHours: occupiedHours ?? this.occupiedHours,
      occupiedSec: occupiedSec ?? this.occupiedSec,
      status: status ?? this.status,
      imageUrl: imageUrl ?? this.imageUrl,
      createdAt: createdAt ?? this.createdAt,
    );
  }
}
