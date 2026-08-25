/// 操作日志条目 (内存态, 记录 App 内用户关键操作, 重启即清空)
class OperationLog {
  final String id;
  final String type; // notify=通知车主 / dispatch=派单 / ota=固件升级 / mode=模式切换
  final String title; // 动作名
  final String? spotId; // 关联车位
  final String detail; // 描述
  final bool success; // 是否成功
  final DateTime createdAt;

  OperationLog({
    required this.id,
    required this.type,
    required this.title,
    this.spotId,
    required this.detail,
    this.success = true,
    required this.createdAt,
  });
}
