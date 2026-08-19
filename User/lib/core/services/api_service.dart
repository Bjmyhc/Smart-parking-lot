import 'dart:convert';
import 'package:http/http.dart' as http;
import 'package:crypto/crypto.dart';
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';

class ApiService {
  static const String _baseUrl = 'https://iot-api.heclouds.com';
  static const String _gatewayProductId = '9YIs0S7V11';
  static const String _nodeProductId = '04kjwU9TC7';
  static const String _userId = '528332';
  static const String _accessKey = 'e3b97243b0d24ffda1befead601ef617';

  final http.Client _client = http.Client();

  String _generateAuthorization() {
    final version = '2020-05-29';
    final res = 'userid/';
    final et = (DateTime.now().millisecondsSinceEpoch ~/ 1000 + 3600).toString();
    final method = 'md5';

    final stringForSignature = '\n\n\n';
    final keyBytes = base64Decode(_accessKey);
    final hmac = Hmac(md5, keyBytes);
    final signature = hmac.convert(utf8.encode(stringForSignature));
    final sign = base64Encode(signature.bytes);

    final encodedRes = Uri.encodeComponent(res);
    final encodedSign = Uri.encodeComponent(sign);

    return 'version=&res=&et=&method=&sign=';
  }

  Future<List<SpotModel>> getSpots() async {
    try {
      final allDevices = <Map<String, dynamic>>[];

      final gatewayDevices = await _fetchProductDevices(_gatewayProductId);
      allDevices.addAll(gatewayDevices);

      final nodeDevices = await _fetchProductDevices(_nodeProductId);
      allDevices.addAll(nodeDevices);

      if (allDevices.isEmpty) {
        return _getMockSpots();
      }

      final devicesWithProps = await _fetchDeviceProperties(allDevices);
      return devicesWithProps.map((e) => SpotModel.fromJson(e)).toList();
    } catch (e) {
      return _getMockSpots();
    }
  }

  Future<List<Map<String, dynamic>>> _fetchProductDevices(String productId) async {
    try {
      final auth = _generateAuthorization();
      final url = Uri.parse('/device/list?product_id=&offset=0&limit=20');

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
          final List<dynamic> rawList = data['data']['list'] ?? [];
          return rawList.map((e) {
            final device = Map<String, dynamic>.from(e as Map);
            device['product_id'] = productId;
            return device;
          }).toList();
        }
      }
      return [];
    } catch (e) {
      return [];
    }
  }

  Future<List<Map<String, dynamic>>> _fetchDeviceProperties(List<Map<String, dynamic>> devices) async {
    final result = <Map<String, dynamic>>[];
    final auth = _generateAuthorization();

    for (final device in devices) {
      final deviceName = device['name'] ?? device['deviceName'] ?? '';
      final productId = device['product_id'] ?? _nodeProductId;

      if (deviceName.isEmpty) {
        result.add(device);
        continue;
      }

      try {
        final url = Uri.parse('/thingmodel/query-device-property?product_id=&device_name=');

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
          }
        }
      } catch (e) {
        // 跳过该设备的属性获取
      }

      result.add(device);
    }

    return result;
  }

  Future<Map<String, dynamic>> getDeviceDetail(String deviceName, {String? productId}) async {
    try {
      final pid = productId ?? _nodeProductId;
      final auth = _generateAuthorization();

      final url = Uri.parse('/thingmodel/query-device-property?product_id=26188&device_name=');

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
      throw Exception('API Error: ');
    } catch (e) {
      return _getMockDeviceDetail(deviceName);
    }
  }

  Future<bool> setProperty(String deviceName, String identifier, dynamic value, {String? productId}) async {
    try {
      final pid = productId ?? _nodeProductId;
      final auth = _generateAuthorization();

      final url = Uri.parse('/thingmodel/set-device-property');

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
            identifier: value is int ? value : value.toString(),
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
    return [
      SpotModel(id: 'Park001', zone: 'A', status: 'occupied', occupiedHours: 2, batteryLevel: 85, signalStrength: -65, plateNumber: '京A·12345'),
      SpotModel(id: 'Park002', zone: 'A', status: 'free', occupiedHours: 0, batteryLevel: 90, signalStrength: -60),
      SpotModel(id: 'Park003', zone: 'A', status: 'zombie', occupiedHours: 72, batteryLevel: 45, signalStrength: -75, plateNumber: '京B·67890'),
      SpotModel(id: 'Park004', zone: 'B', status: 'occupied', occupiedHours: 5, batteryLevel: 80, signalStrength: -70, plateNumber: '京C·11111'),
      SpotModel(id: 'Park005', zone: 'B', status: 'free', occupiedHours: 0, batteryLevel: 88, signalStrength: -62),
      SpotModel(id: 'Park006', zone: 'B', status: 'occupied', occupiedHours: 1, batteryLevel: 92, signalStrength: -58, plateNumber: '京D·22222'),
    ];
  }

  List<AlertModel> _getMockAlerts() {
    return [
      AlertModel(id: 'alert_1', plateNumber: '京B·67890', spotId: 'Park003', occupiedHours: 72, status: 'pending', createdAt: DateTime.now().subtract(const Duration(hours: 72))),
      AlertModel(id: 'alert_2', plateNumber: '京E·33333', spotId: 'Park010', occupiedHours: 48, status: 'dispatched', createdAt: DateTime.now().subtract(const Duration(hours: 48))),
      AlertModel(id: 'alert_3', plateNumber: '京F·44444', spotId: 'Park015', occupiedHours: 96, status: 'resolved', createdAt: DateTime.now().subtract(const Duration(hours: 96))),
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