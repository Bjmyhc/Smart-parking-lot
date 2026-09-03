import 'spot_model.dart';

/// 网关设备模型 (设备中心多网关展示)
///
/// `isReal=true` 表示平台真实网关(数据来自 OneNET device/list),
/// `isReal=false` 表示本地模拟网关(模拟模式下由车位列表派生, 用于演示多网关架构).
class GatewayModel {
  final String name;       // 网关设备名 (如 PGW001)
  final bool online;       // 网关在线状态
  final bool isDisabled;   // 是否平台停用
  final bool isReal;       // 是否平台真实网关
  final List<SpotModel> children; // 下挂子设备列表

  GatewayModel({
    required this.name,
    required this.online,
    required this.isDisabled,
    this.isReal = true,
    required this.children,
  });

  /// 从 OneNET device/list 返回的原始 map 构造 (不含子设备, children 稍后填充)
  factory GatewayModel.fromMap(Map<String, dynamic> map) {
    final name = (map['name'] ?? map['deviceName'] ?? '').toString();
    final statusValue = map['status'];
    final online = map['online'] == true ||
        statusValue == 1 ||
        statusValue == 'online' ||
        statusValue == true;
    final enabled = map['enable_status'];
    final isDisabled = enabled == false || enabled == 'false' || map['is_disabled'] == true;
    return GatewayModel(
      name: name,
      online: online,
      isDisabled: isDisabled,
      isReal: true,
      children: const [],
    );
  }

  /// 子设备在线数 (停用节点不计入在线)
  int get onlineChildren => children.where((s) => s.isOnline && !s.isDisabledSpot).length;

  /// 从设备名提取末尾数字 (PGW001 -> 1, Park003 -> 3), 用于排序/编号展示
  static int numericId(String id) {
    final match = RegExp(r'(\d+)$').firstMatch(id);
    return match != null ? int.parse(match.group(1)!) : -1;
  }

  /// 构造模拟网关列表: 把车位按编号均匀分配到 [count] 个模拟网关
  /// (模拟模式下用于演示多网关架构, 数据来自本地车位列表, 不调用平台 API)
  static List<GatewayModel> buildSimulated(List<SpotModel> spots, {int count = 3}) {
    final sorted = [...spots]
      ..sort((a, b) => numericId(a.id).compareTo(numericId(b.id)));
    final n = sorted.length;
    final result = <GatewayModel>[];
    for (var i = 0; i < count; i++) {
      final start = (i * n) ~/ count;
      final end = ((i + 1) * n) ~/ count;
      final chunk = sorted.sublist(start, end);
      final name = 'PGW${(i + 1).toString().padLeft(3, '0')}';
      result.add(GatewayModel(
        name: name,
        online: true,
        isDisabled: false,
        isReal: false,
        children: chunk,
      ));
    }
    // 过滤掉空网关 (车位不足时可能产生空 chunk)
    return result.where((g) => g.children.isNotEmpty).toList();
  }
}
