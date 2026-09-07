"""
OneNET 云平台 HTTP API 封装
===========================
提供停车场项目的设备属性查询/设置能力, 供 MCP 工具调用.

接口(与 web前端 app.js 保持一致):
  - GET  https://iot-api.heclouds.com/thingmodel/query-device-property
        ?product_id=<产品ID>&device_name=<设备名>
        header: authorization=<token>
        返回设备最新属性快照
  - POST https://iot-api.heclouds.com/thingmodel/call-service
        body: {product_id, device_name, identifier, params}
        同步调用设备物模型服务(如 SetLed), 平台等待设备返回结果

鉴权:
  token 在 OneNET 控制台 -> 设备详情 -> 鉴权信息/APIKey 中生成,
  格式形如:
    version=2018-10-31&res=products%2F<产品ID>%2Fdevices%2F<设备名>&et=<过期时间>&method=md5&sign=<签名>
"""

import requests

API_BASE = "https://iot-api.heclouds.com"

# 物模型属性中文说明(仅用于展示, 与平台物模型标识符保持一致)
PROPERTY_LABELS = {
    "ParkStatus":        "车位状态",
    "GeoMagnetic":       "地磁检测",
    "Ultrasonic":        "超声波距离",
    "OccupiedTime":      "占用时长",
    "LED":               "LED当前状态",
    "SensorDistanceCm":  "超声波判定距离阈值",
    "ZombieThresholdSec": "僵尸车判定阈值",
    "SignalRssi":        "信号强度",
}

# 车位状态数值 -> 中文
PARK_STATUS_TEXT = {
    0: "空闲",
    1: "有车",
    2: "疑似僵尸车",
}

# 设备在线状态数值 -> 中文 (device/detail 接口返回 data.status)
DEVICE_STATUS_TEXT = {
    0: "离线",
    1: "在线",
    2: "未激活",
}

# 设备"停用"状态: 平台侧设备启/停用, 由 device/detail 返回的 data.enable_status 判断
DEVICE_STATUS_DISABLED = "停用"


def extract_value(v):
    """取属性值: 兼容 {value: xxx} 包装对象与裸值两种格式"""
    if isinstance(v, dict) and "value" in v:
        return v["value"]
    return v


class OneNetClient:
    """单个 OneNET 设备的查询/控制客户端"""

    def __init__(self, product_id, device_name, token, timeout=10):
        self.product_id = product_id
        self.device_name = device_name
        self.token = token
        self.timeout = timeout
        self.headers = {"authorization": token}

    def query_property(self):
        """查询设备最新属性快照, 返回 {标识符: 值} 字典"""
        url = f"{API_BASE}/thingmodel/query-device-property"
        params = {"product_id": self.product_id, "device_name": self.device_name}
        resp = requests.get(url, params=params, headers=self.headers,
                            timeout=self.timeout)
        resp.raise_for_status()
        return self._parse_props(resp.json())

    def query_device_status(self):
        """查询设备状态, 返回中文状态(在线/离线/未激活/停用)

        属性快照接口(thingmodel/query-device-property)不含在线状态,
        离线设备的快照会停留在最后一次上报值, 导致被误判为正常;
        需调用 GET /device/detail 接口:
          - data.enable_status == False => 设备已在平台被停用
          - data.status: 0=离线 / 1=在线 / 2=未激活
        """
        url = f"{API_BASE}/device/detail"
        params = {"product_id": self.product_id, "device_name": self.device_name}
        resp = requests.get(url, params=params, headers=self.headers,
                            timeout=self.timeout)
        resp.raise_for_status()
        data = resp.json()
        dev = data.get("data") or {}
        if dev.get("enable_status") is False:
            return DEVICE_STATUS_DISABLED
        status = dev.get("status")
        return DEVICE_STATUS_TEXT.get(status, f"未知({status})")

    def call_service(self, identifier, params, timeout=None):
        """同步调用设备物模型服务, 返回平台响应 JSON.

        平台将服务调用命令下发到设备(经网关转发), 并同步等待设备处理结果:
        - 成功: code=0, data 为服务输出参数(如 SetLed 返回 {Result, ActualValue})
        - 失败: code 非 0, msg 描述错误(如设备离线/超时/参数越界)

        例: call_service("SetLed", {"LedState": True})
        """
        url = f"{API_BASE}/thingmodel/call-service"
        payload = {
            "product_id": self.product_id,
            "device_name": self.device_name,
            "identifier": identifier,
            "params": params,
        }
        # 同步调用要等设备回复(网关截止9s + 平台处理), 默认给足 20s
        resp = requests.post(url, json=payload, headers=self.headers,
                             timeout=timeout or max(self.timeout, 20))
        resp.raise_for_status()
        return resp.json()

    @staticmethod
    def _parse_props(data):
        """兼容 web前端 的多种响应结构, 统一成 {标识符: 值} 字典"""
        container = data.get("data", data) if isinstance(data, dict) else data

        items = None
        if isinstance(container, list):
            items = container
        elif isinstance(container, dict):
            for key in ("items", "properties"):
                if isinstance(container.get(key), list):
                    items = container[key]
                    break
            if items is None and isinstance(data, dict):
                for key in ("items", "properties"):
                    if isinstance(data.get(key), list):
                        items = data[key]
                        break

        props = {}
        if items is not None:
            for item in items:
                if isinstance(item, dict) and "identifier" in item:
                    props[item["identifier"]] = extract_value(item.get("value"))
        elif isinstance(container, dict):
            for key, val in container.items():
                if key in ("items", "properties"):
                    continue
                props[key] = extract_value(val)
        return props

    @staticmethod
    def format_props(props):
        """把属性字典格式化成人类可读文本(给 LLM/语音助手看)"""
        lines = []
        for identifier, value in props.items():
            label = PROPERTY_LABELS.get(identifier, identifier)
            if identifier == "ParkStatus":
                try:
                    value = PARK_STATUS_TEXT.get(int(value), value)
                except (TypeError, ValueError):
                    pass
            elif identifier == "OccupiedTime":
                try:
                    value = f"{int(value)}秒"
                except (TypeError, ValueError):
                    pass
            elif identifier == "LED":
                value = "开" if value in (True, 1, "true", "1") else "关"
            elif identifier == "Ultrasonic":
                try:
                    value = f"{int(value)}cm"
                except (TypeError, ValueError):
                    pass
            elif identifier == "SensorDistanceCm":
                try:
                    value = f"{int(value)}cm"
                except (TypeError, ValueError):
                    pass
            elif identifier == "ZombieThresholdSec":
                try:
                    value = f"{int(value)}秒"
                except (TypeError, ValueError):
                    pass
            lines.append(f"{label}={value}")
        return ", ".join(lines) if lines else "无属性数据"
