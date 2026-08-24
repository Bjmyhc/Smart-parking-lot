import 'dart:convert';
import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';

class ApiService {
  static const String _baseUrl = 'https://iot-api.heclouds.com';
  // 车位列表只展示节点产品(04kjwU9TC7)下的设备; 网关产品(9YIs0S7V11)不是车位, 不纳入列表
  static const String _nodeProductId = '04kjwU9TC7';
  // 网关设备(承载 OtaAllow 全网升级确认门控): OtaAllow 是网关自身属性, 下发目标为 PGW001
  static const String gatewayProductId = '9YIs0S7V11';
  static const String gatewayDeviceId = 'PGW001';
  static const String _userId = '528332';
  static const String _accessKey = 'e3b97243b0d24ffda1befead601ef617';

  final http.Client _client = http.Client();

  String _generateAuthorization() {
    const version = '2020-05-29';
    // 主用户资源标识: userid/{userid}，末尾不带斜杠
    const res = 'userid/$_userId';
    final et = (DateTime.now().millisecondsSinceEpoch ~/ 1000 + 3600).toString();
    const method = 'md5';

    // 官方规范: StringForSignature = et + "\n" + method + "\n" + res + "\n" + version (末尾无换行)
    final stringToSign = '$et\n$method\n$res\n$version';
    debugPrint('>>> StringToSign: $stringToSign');

    final keyBytes = base64Decode(_accessKey);
    final hmac = Hmac(md5, keyBytes);
    final signature = hmac.convert(utf8.encode(stringToSign));
    final sign = base64Encode(signature.bytes);

    final encodedRes = Uri.encodeComponent(res);
    final encodedSign = Uri.encodeComponent(sign);

    final auth = 'version=$version&res=$encodedRes&et=$et&method=$method&sign=$encodedSign';
    debugPrint('>>> Authorization: $auth');
    return auth;
  }

  Future<List<SpotModel>> getSpots({bool realOnly = false}) async {
    List<SpotModel> realSpots = [];
    try {
      // 只取节点产品下的设备作为车位; 网关(如 PGW001)不是车位, 不纳入车位列表
      final nodeDevices = await _fetchProductDevices(_nodeProductId);
      if (nodeDevices.isNotEmpty) {
        final devicesWithProps = await _fetchDeviceProperties(nodeDevices);
        final parsed = <SpotModel>[];
        for (final e in devicesWithProps) {
          try {
            parsed.add(SpotModel.fromJson(e));
          } catch (err) {
            // 单台设备解析失败不影响其它设备
            debugPrint('>>> 单台设备解析失败: ${e['name']} -> $err');
          }
        }
        realSpots = parsed;
        debugPrint('>>> 真实设备: ${realSpots.length} 台');
      }
    } catch (e) {
      debugPrint('>>> 获取真实设备失败: $e');
    }

    // 按车位编号升序排列, 保证 001 在 002 之前
    realSpots.sort((a, b) => _numericId(a.id).compareTo(_numericId(b.id)));

    // 真实模式: 只显示真实设备, 不补模拟数据
    if (realOnly) {
      debugPrint('>>> 真实模式: 仅展示 ${realSpots.length} 台真实设备');
      return realSpots;
    }

    // 模拟设备补齐到 9 台: 去掉编号与真实车位重复的(如真实 Park001 视同 001), 避免出现两个 001
    final realNums = realSpots.map((s) => _numericId(s.id)).toSet();
    final remaining = 9 - realSpots.length;
    final extraSpots = _getMockSpots()
        .where((s) => !realNums.contains(_numericId(s.id)))
        .take(remaining > 0 ? remaining : 0)
        .toList();

    final allSpots = [...realSpots, ...extraSpots];
    debugPrint('>>> 合计: ${allSpots.length} 台 (真实${realSpots.length} + 模拟${extraSpots.length})');
    return allSpots;
  }

  /// 提取车位编号末尾的数字, 用于排序与去重 (Park001 → 1, 001 → 1)
  int _numericId(String id) {
    final match = RegExp(r'(\d+)$').firstMatch(id);
    return match != null ? int.parse(match.group(1)!) : -1;
  }

  Future<List<Map<String, dynamic>>> _fetchProductDevices(String productId) async {
    try {
      final auth = _generateAuthorization();
      final url = Uri.parse('$_baseUrl/device/list?product_id=$productId&offset=0&limit=20');
      debugPrint('>>> OneNET 请求: $url');

      final response = await _client.get(
        url,
        headers: {
          'Authorization': auth,
          'Content-Type': 'application/json',
        },
      );

      debugPrint('>>> HTTP ${response.statusCode}');
      debugPrint('>>> 响应体: ${response.body}');

      if (response.statusCode == 200) {
        final data = json.decode(response.body);
        if (data['code'] == 0) {
          final List<dynamic> rawList = data['data']['list'] ?? [];
          debugPrint('>>> 设备数量: ${rawList.length}');
          final devices = <Map<String, dynamic>>[];
          for (var d in rawList) {
            final device = Map<String, dynamic>.from(d as Map);
            device['product_id'] = productId;
            // OneNET 的 status 是 int: 1=在线, 0=离线; 也兼容 "online"/"offline" 字符串
            final statusValue = device['status'];
            final isOnline = statusValue == 1 || statusValue == 'online' || statusValue == true;
            device['online'] = isOnline;
            debugPrint('>>> 设备: ${device['name']} (${isOnline ? "在线" : "离线"})');
            devices.add(device);
          }
          return devices;
        } else {
          debugPrint('>>> OneNET 返回错误: code=${data['code']}, msg=${data['msg']}');
        }
      }
      return [];
    } catch (e) {
      debugPrint('>>> 请求异常: $e');
      return [];
    }
  }

  Future<List<Map<String, dynamic>>> _fetchDeviceProperties(List<Map<String, dynamic>> devices) async {
    final result = <Map<String, dynamic>>[];

    for (final device in devices) {
      final deviceName = device['name'] ?? device['deviceName'] ?? '';
      final productId = device['product_id'] ?? _nodeProductId;
      final isOnline = device['online'] == true;

      if (deviceName.isEmpty) {
        device['online'] = false;
        result.add(device);
        continue;
      }

      if (!isOnline) {
        // 必须用带类型的空 Map，否则 {} 会被推断为 Map<dynamic, dynamic>,
        // 导致 SpotModel.fromJson 里 `as Map<String, dynamic>?` 强转失败
        device['properties'] = <String, dynamic>{};
        device['updated_at'] = device['last_time'] ?? '';
        result.add(device);
        continue;
      }

      try {
        final auth = _generateAuthorization();
        final url = Uri.parse('$_baseUrl/thingmodel/query-device-property?product_id=$productId&device_name=${Uri.encodeComponent(deviceName)}');

        final response = await _client.get(
          url,
          headers: {
            'Authorization': auth,
            'Content-Type': 'application/json',
          },
        );

        if (response.statusCode == 200) {
          final data = json.decode(response.body);
          debugPrint('>>> 属性查询 ${device['name']}: ${data['code']} ${data['msg']}');
          debugPrint('>>> 属性响应体: ${response.body}');
          if (data['code'] == 0) {
            final properties = <String, dynamic>{};
            final List<dynamic> propList = data['data'] ?? [];

            for (final prop in propList) {
              final identifier = prop['identifier'] as String? ?? '';
              final value = prop['value'];
              if (identifier.isNotEmpty) {
                if (value is String) {
                  if (value == 'true') {
                    properties[identifier] = true;
                  } else if (value == 'false') {
                    properties[identifier] = false;
                  } else {
                    properties[identifier] = int.tryParse(value) ?? value;
                  }
                } else {
                  properties[identifier] = value;
                }
              }
            }

            device['properties'] = properties;
            device['updated_at'] = DateTime.now().toIso8601String();
            device['online'] = true;
          } else {
            device['online'] = false;
          }
        } else {
          device['online'] = false;
        }
      } catch (e) {
        device['online'] = false;
      }

      result.add(device);
    }

    return result;
  }

  Future<Map<String, dynamic>> getDeviceDetail(String deviceName, {String? productId}) async {
    try {
      final pid = productId ?? _nodeProductId;
      final auth = _generateAuthorization();
      final url = Uri.parse('$_baseUrl/thingmodel/query-device-property?product_id=$pid&device_name=${Uri.encodeComponent(deviceName)}');

      final response = await _client.get(
        url,
        headers: {
          'Authorization': auth,
          'Content-Type': 'application/json',
        },
      );

      if (response.statusCode == 200) {
        final data = json.decode(response.body);
        if (data['code'] == 0) {
          final result = <String, dynamic>{
            'deviceName': deviceName,
            'product_id': pid,
          };

          final properties = <String, dynamic>{};
          final List<dynamic> propList = data['data'] ?? [];

          for (final prop in propList) {
            final identifier = prop['identifier'] as String? ?? '';
            final value = prop['value'];
            if (identifier.isNotEmpty) {
              if (value is String) {
                if (value == 'true') {
                  properties[identifier] = true;
                } else if (value == 'false') {
                  properties[identifier] = false;
                } else {
                  properties[identifier] = int.tryParse(value) ?? value;
                }
              } else {
                properties[identifier] = value;
              }
            }
          }

          result['properties'] = properties;
          result['updated_at'] = DateTime.now().toIso8601String();
          return result;
        }
      }
      throw Exception('API Error: ${response.statusCode}');
    } catch (e) {
      return _getMockDeviceDetail(deviceName);
    }
  }

  Future<bool> setProperty(String deviceName, String identifier, dynamic value, {String? productId}) async {
    try {
      final pid = productId ?? _nodeProductId;
      final auth = _generateAuthorization();

      final url = Uri.parse('$_baseUrl/thingmodel/set-device-property');

      final response = await _client.post(
        url,
        headers: {
          'Authorization': auth,
          'Content-Type': 'application/json',
        },
        body: json.encode({
          'product_id': pid,
          'device_name': deviceName,
          'params': {
            // 保留原始类型: 布尔属性发 true/false, 整型属性发数字, 避免平台类型校验拒绝
            identifier: value,
          },
        }),
      );

      if (response.statusCode == 200) {
        final data = json.decode(response.body);
        return data['code'] == 0;
      }
      return false;
    } catch (e) {
      return true;
    }
  }

  /* ==================== OneNET 固件升级 (fuse-ota, sha1/2022-05-01) ====================
   * 升级任务检测/状态查询用"新版"签名: method=sha1, version=2022-05-01,
   * res=userid/{userId} (用户级 accessKey, 与网关 SOTA 一致), 可跨产品访问节点 OTA.
   * 全网一键升级确认/复位复用现有 setProperty 下发 OtaAllow(1=确认 / 0=复位). */

  static const String _otaVersion = '2022-05-01';
  static const String _otaMethod = 'sha1';

  String _generateOtaAuthorization() {
    // OneNET 规范: StringForSignature = et + "\n" + method + "\n" + res + "\n" + version
    const res = 'userid/$_userId';
    final et = (DateTime.now().millisecondsSinceEpoch ~/ 1000 + 300).toString();
    final stringToSign = '$et\n$_otaMethod\n$res\n$_otaVersion';
    debugPrint('>>> OTA StringToSign: $stringToSign');

    final keyBytes = base64Decode(_accessKey);
    final hmac = Hmac(sha1, keyBytes);
    final signature = hmac.convert(utf8.encode(stringToSign));
    final sign = base64Encode(signature.bytes);

    final auth = 'version=$_otaVersion&res=${Uri.encodeComponent(res)}&et=$et&method=$_otaMethod&sign=${Uri.encodeComponent(sign)}';
    debugPrint('>>> OTA Authorization: $auth');
    return auth;
  }

  /// 读取节点当前固件版本 (GET /fuse-ota/{pro}/{dev}/version), 返回 s_version; 失败返回 null.
  Future<String?> getOtaNodeVersion(String deviceName) async {
    try {
      final auth = _generateOtaAuthorization();
      final url = Uri.parse('$_baseUrl/fuse-ota/$_nodeProductId/${Uri.encodeComponent(deviceName)}/version');
      final response = await _client.get(
        url,
        headers: {'Authorization': auth, 'Content-Type': 'application/json'},
      );
      debugPrint('>>> OTA 读版本 HTTP ${response.statusCode}: ${response.body}');
      if (response.statusCode != 200) return null;
      final data = json.decode(response.body);
      if (data['code'] != 0) return null;
      final obj = data['data'];
      if (obj is Map) {
        final v = obj['s_version'];
        if (v != null) return v.toString();
      }
      return null;
    } catch (e) {
      debugPrint('>>> OTA 读版本异常: $e');
      return null;
    }
  }

  /// 检测节点升级任务 (GET /fuse-ota/{pro}/{dev}/check?type=2&version={cur}).
  /// 有未结束任务返回 {tid,target,size,status}, 无任务/已结束返回 null.
  Future<Map<String, dynamic>?> getOtaTask(String deviceName, {required String version}) async {
    try {
      final auth = _generateOtaAuthorization();
      final url = Uri.parse('$_baseUrl/fuse-ota/$_nodeProductId/${Uri.encodeComponent(deviceName)}/check?type=2&version=${Uri.encodeComponent(version)}');
      final response = await _client.get(
        url,
        headers: {'Authorization': auth, 'Content-Type': 'application/json'},
      );
      debugPrint('>>> OTA 查任务 HTTP ${response.statusCode}: ${response.body}');
      if (response.statusCode != 200) return null;
      final data = json.decode(response.body);
      if (data['code'] != 0) return null;
      final task = data['data'];
      if (task is! Map || task.isEmpty) return null;
      final status = task['status'];
      // 平台对已结束任务(status>3)不再作为"待升级任务"返回
      if (status is int && status > 3) return null;
      return Map<String, dynamic>.from(task);
    } catch (e) {
      debugPrint('>>> OTA 查任务异常: $e');
      return null;
    }
  }

  /// 查询指定升级任务的状态 (GET /fuse-ota/{pro}/{dev}/{tid}/check).
  /// 官方返回 data:{status}, 1待升级 2下载中 3升级中 4升级成功 5失败 6取消.
  /// 任务状态独立且持久: 完成/失败后保持 status=4/5, 不会像物模型属性那样残留,
  /// 天然避免"下次升级误读旧状态". 失败/接口异常返回 null (保留上一轮状态).
  Future<int?> getOtaTaskStatus(String deviceName, String tid) async {
    try {
      final auth = _generateOtaAuthorization();
      final url = Uri.parse(
          '$_baseUrl/fuse-ota/$_nodeProductId/${Uri.encodeComponent(deviceName)}/$tid/check');
      final response = await _client.get(
        url,
        headers: {'Authorization': auth, 'Content-Type': 'application/json'},
      );
      debugPrint('>>> OTA 查任务状态 HTTP ${response.statusCode}: ${response.body}');
      if (response.statusCode != 200) return null;
      final data = json.decode(response.body);
      if (data['code'] != 0) return null;
      final obj = data['data'];
      if (obj is Map) {
        final s = obj['status'];
        if (s is num) return s.toInt();
        if (s is String) return int.tryParse(s);
      }
      return null;
    } catch (e) {
      debugPrint('>>> OTA 查任务状态异常: $e');
      return null;
    }
  }

  /// 读取网关 OTA 实时进度 (网关自身物模型属性 OtaProgress, 由网关经 MQTT 上报到平台).
  /// 阶段状态已改用官方 fuse-ota $tid/check 接口, 这里只取真实进度驱动精确进度条.
  /// 返回 {OtaProgress:int}; 网关未上报/请求失败返回 null.
  Future<Map<String, dynamic>?> getGatewayOtaState() async {
    try {
      final auth = _generateAuthorization();
      final url = Uri.parse('$_baseUrl/thingmodel/query-device-property?product_id=$gatewayProductId&device_name=${Uri.encodeComponent(gatewayDeviceId)}');
      final response = await _client.get(
        url,
        headers: {'Authorization': auth, 'Content-Type': 'application/json'},
      );
      debugPrint('>>> OTA 读网关进度 HTTP ${response.statusCode}: ${response.body}');
      if (response.statusCode != 200) return null;
      final data = json.decode(response.body);
      if (data['code'] != 0) return null;

      final result = <String, dynamic>{};
      final List<dynamic> propList = data['data'] ?? [];
      for (final prop in propList) {
        final identifier = prop['identifier'] as String? ?? '';
        final value = prop['value'];
        if (identifier == 'OtaProgress') {
          if (value is num) {
            result[identifier] = value.toInt();
          } else if (value is String) {
            result[identifier] = int.tryParse(value) ?? 0;
          }
        }
      }
      if (result.isEmpty) return null; // 属性未上报(网关未升级固件/未配置物模型)
      return result;
    } catch (e) {
      debugPrint('>>> OTA 读网关进度异常: $e');
      return null;
    }
  }

  Future<List<AlertModel>> getAlerts() async {
    try {
      final spots = await getSpots();
      final alerts = <AlertModel>[];

      for (final spot in spots) {
        if (spot.isOccupied && spot.occupiedHours >= 24) {
          alerts.add(AlertModel(
            id: 'alert_',
            plateNumber: spot.plateNumber ?? '未知车牌',
            spotId: spot.id,
            occupiedHours: spot.occupiedHours,
            status: spot.occupiedHours >= 72 ? 'pending' : 'dispatched',
            createdAt: DateTime.now().subtract(Duration(hours: spot.occupiedHours)),
          ));
        }
      }

      if (alerts.isEmpty) {
        return _getMockAlerts();
      }

      return alerts;
    } catch (e) {
      return _getMockAlerts();
    }
  }

  Future<bool> dispatchAlert(String alertId) async {
    await Future.delayed(const Duration(seconds: 1));
    return true;
  }

  Future<bool> notifyOwner(String alertId) async {
    await Future.delayed(const Duration(seconds: 1));
    return true;
  }

  Future<bool> resolveAlert(String alertId) async {
    await Future.delayed(const Duration(seconds: 1));
    return true;
  }

  Future<StatsModel> getStats() async {
    try {
      final spots = await getSpots();
      final total = spots.length;
      final occupied = spots.where((s) => s.isOccupied).length;
      final zombie = spots.where((s) => s.isZombie).length;
      final rate = total > 0 ? occupied / total : 0.0;

      return StatsModel(
        totalSpots: total,
        occupiedSpots: occupied,
        zombieSpots: zombie,
        occupancyRate: rate,
        weeklyTrend: _getMockWeeklyTrend(),
      );
    } catch (e) {
      return StatsModel.empty();
    }
  }

  List<SpotModel> _getMockSpots() {
    // 模拟车位编号与真实设备命名一致 (Park00x), 便于真实/本地模式统一展示
    return [
      SpotModel(id: 'Park001', zone: 'A', status: 'occupied', occupiedHours: 2, batteryLevel: 85, signalStrength: -65, plateNumber: '京A·12345'),
      SpotModel(id: 'Park002', zone: 'A', status: 'free', occupiedHours: 0, batteryLevel: 90, signalStrength: -60),
      SpotModel(id: 'Park003', zone: 'A', status: 'zombie', occupiedHours: 72, batteryLevel: 45, signalStrength: -75, plateNumber: '京B·67890'),
      SpotModel(id: 'Park004', zone: 'B', status: 'occupied', occupiedHours: 5, batteryLevel: 80, signalStrength: -70, plateNumber: '京C·11111'),
      SpotModel(id: 'Park005', zone: 'B', status: 'free', occupiedHours: 0, batteryLevel: 88, signalStrength: -62),
      SpotModel(id: 'Park006', zone: 'B', status: 'occupied', occupiedHours: 1, batteryLevel: 92, signalStrength: -58, plateNumber: '京D·22222'),
      SpotModel(id: 'Park007', zone: 'C', status: 'offline', isOnline: false),
      SpotModel(id: 'Park008', zone: 'C', status: 'free', occupiedHours: 0, batteryLevel: 95, signalStrength: -55),
      SpotModel(id: 'Park009', zone: 'C', status: 'occupied', occupiedHours: 8, batteryLevel: 78, signalStrength: -80, plateNumber: '京E·33333'),
    ];
  }

  List<AlertModel> _getMockAlerts() {
    return [
      AlertModel(id: 'alert_1', plateNumber: '京B·67890', spotId: 'Park003', occupiedHours: 72, status: 'pending', createdAt: DateTime.now().subtract(const Duration(hours: 72))),
      AlertModel(id: 'alert_2', plateNumber: '京E·33333', spotId: 'Park009', occupiedHours: 48, status: 'dispatched', createdAt: DateTime.now().subtract(const Duration(hours: 48))),
      AlertModel(id: 'alert_3', plateNumber: '京F·44444', spotId: 'Park010', occupiedHours: 96, status: 'resolved', createdAt: DateTime.now().subtract(const Duration(hours: 96))),
    ];
  }

  List<DailyTrend> _getMockWeeklyTrend() {
    return [
      DailyTrend(date: '周一', avgOccupancy: 45, alertsCount: 2),
      DailyTrend(date: '周二', avgOccupancy: 52, alertsCount: 1),
      DailyTrend(date: '周三', avgOccupancy: 48, alertsCount: 3),
      DailyTrend(date: '周四', avgOccupancy: 60, alertsCount: 0),
      DailyTrend(date: '周五', avgOccupancy: 55, alertsCount: 2),
      DailyTrend(date: '周六', avgOccupancy: 68, alertsCount: 1),
      DailyTrend(date: '周日', avgOccupancy: 58, alertsCount: 2),
    ];
  }

  Map<String, dynamic> _getMockDeviceDetail(String deviceName) {
    return {
      'deviceName': deviceName,
      'product_id': _nodeProductId,
      'properties': {
        'ParkStatus': 0,
        'GeoMagnetic': 0,
        'Ultrasonic': 350,
        'OccupiedTime': 0,
        'LED': 0,
        'LedEnable': 1,
      },
      'updated_at': DateTime.now().toIso8601String(),
    };
  }

  void dispose() {
    _client.close();
  }
}
