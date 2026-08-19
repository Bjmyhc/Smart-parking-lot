class AlertModel {
  final String id;
  final String plateNumber;
  final String spotId;
  final int occupiedHours;
  final String status;
  final String? imageUrl;
  final DateTime? createdAt;

  AlertModel({
    required this.id,
    required this.plateNumber,
    required this.spotId,
    required this.occupiedHours,
    this.status = 'pending',
    this.imageUrl,
    this.createdAt,
  });

  factory AlertModel.fromJson(Map<String, dynamic> json) {
    return AlertModel(
      id: json['id'] as String? ?? '',
      plateNumber: json['plate_number'] as String? ?? '',
      spotId: json['spot_id'] as String? ?? '',
      occupiedHours: (json['occupied_hours'] as num?)?.toInt() ?? 0,
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
      'spot_id': spotId,
      'occupied_hours': occupiedHours,
      'status': status,
      'image_url': imageUrl,
      'created_at': createdAt?.toIso8601String(),
    };
  }

  bool get isPending => status == 'pending';
  bool get isDispatched => status == 'dispatched';
  bool get isResolved => status == 'resolved';
}
