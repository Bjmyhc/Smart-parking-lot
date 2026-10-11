"""
智能停车场 MCP 服务
==================
让"小智 AI"等 MCP 客户端通过本服务实时查询停车场状态、远程控制设备.

数据链路: STM32节点 -> LoRa -> ESP8266网关 -> OneNET云平台 -> 本 MCP 工具
(直接调用 OneNET HTTP API, 网关/节点代码无需任何改动)

提供工具:
  list_nodes()            列出所有已配置的车位节点
  query_node_status()     查询单个节点实时状态
  query_all_nodes()       查询所有节点实时状态
  get_parking_summary()   停车场概览(空闲/有车/僵尸车/离线统计)
  query_plate()           查询车牌识别结果(牌号/置信度/颜色/摄像头/缩略图状态)
  get_plate_image()       车牌缩略图 ASCII 点阵(94×24字符画, 供核对拍摄内容)
  take_photo()            远程触发拍照识别(TriggerCapture)
  set_led_enable()        同步服务调用: 远程控制某节点 LED 使能开关
  set_zombie_threshold()  同步服务调用: 设置某节点僵尸车判定阈值(秒)
  set_sensor_distance()   同步服务调用: 设置某节点超声波判定距离阈值(cm)
  set_capture_policy()    同步服务调用: 设置拍照策略(0~3档)

运行方式:
  stdio(默认):   python parking_mcp_server.py
  sse(网络):     python parking_mcp_server.py --transport sse --port 8000

配置文件: config.json (由 config.example.json 复制并填写各设备 token 生成)
"""

import argparse
import json
import os

from mcp.server.fastmcp import FastMCP
from onenet_client import OneNetClient, render_thumb_ascii, PLATE_COLOR_TEXT

# 配置文件路径(与脚本同目录)
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

mcp = FastMCP("智能停车场")


def load_config():
    """读取 config.json, 缺失时给出友好提示"""
    if not os.path.exists(CONFIG_FILE):
        raise RuntimeError(
            "缺少 config.json! 请复制 config.example.json 为 config.json, "
            "并按说明填写各设备 token 后重试"
        )
    with open(CONFIG_FILE, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def get_client(cfg, node_name):
    """按节点名找到 OneNET 客户端"""
    for node in cfg.get("nodes", []):
        if node.get("name") == node_name:
            return OneNetClient(
                node["product_id"],
                node["device_name"],
                node["token"],
                timeout=cfg.get("query_timeout", 10),
            )
    names = [n.get("name") for n in cfg.get("nodes", [])]
    raise ValueError(f"未找到节点 '{node_name}', 可用节点: {names}")


@mcp.tool()
def list_nodes() -> str:
    """列出所有已配置的车位节点及设备信息(设备管理用)"""
    cfg = load_config()
    rows = []
    for node in cfg.get("nodes", []):
        rows.append(
            f"- {node.get('name')}: 产品ID={node.get('product_id')}, "
            f"设备名={node.get('device_name')}"
        )
    if not rows:
        return "未配置任何节点"
    return "已配置节点:\n" + "\n".join(rows)


@mcp.tool()
def query_node_status(node_name: str) -> str:
    """查询指定车位节点的实时状态(在线状态/停车状态/超声波距离/地磁/占用时长/LED), node_name 如 Park001"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        status = client.query_device_status()
    except Exception as exc:  # 网络/平台异常
        return f"查询节点 {node_name} 失败: {exc}"
    if status != "在线":
        # 设备不在线时, 平台属性快照停留在最后一次上报值(如 ParkStatus=0 空闲),
        # 属陈旧数据不代表当前真实状态, 只报设备状态避免误导
        return f"节点 {node_name}: 状态={status}"
    try:
        props = client.query_property()
    except Exception as exc:
        return f"节点 {node_name}: 在线但属性查询失败({exc})"
    return f"节点 {node_name}: 状态=在线, {OneNetClient.format_props(props)}"


@mcp.tool()
def query_all_nodes() -> str:
    """查询所有已配置车位节点的实时状态(含在线状态)"""
    cfg = load_config()
    parts = []
    for node in cfg.get("nodes", []):
        client = OneNetClient(
            node["product_id"], node["device_name"], node["token"],
            timeout=cfg.get("query_timeout", 10),
        )
        try:
            status = client.query_device_status()
            if status != "在线":
                # 离线/停用/未激活设备不展示陈旧属性快照
                parts.append(f"{node['name']}: 状态={status}")
                continue
            props = client.query_property()
            parts.append(f"{node['name']}: 状态=在线, {OneNetClient.format_props(props)}")
        except Exception as exc:
            parts.append(f"{node['name']}: 查询失败({exc})")
    return "\n".join(parts) if parts else "未配置任何节点"


@mcp.tool()
def get_parking_summary() -> str:
    """停车场概览: 统计各车位占用情况(空闲/有车/僵尸车/离线/停用), 供快速回答'停车场现在什么情况'"""
    cfg = load_config()
    stats = {"空闲": 0, "有车": 0, "疑似僵尸车": 0, "离线": 0, "停用": 0, "未激活": 0, "未知": 0}
    details = []
    for node in cfg.get("nodes", []):
        client = OneNetClient(
            node["product_id"], node["device_name"], node["token"],
            timeout=cfg.get("query_timeout", 10),
        )
        try:
            # 先查设备状态: 离线/停用/未激活的设备, 其属性快照会停留在最后一次上报值, 不能当作正常车位统计
            dev_state = client.query_device_status()
            if dev_state != "在线":
                stats[dev_state] += 1
                details.append(f"{node['name']}: {dev_state}")
                continue
            props = client.query_property()
            status = props.get("ParkStatus")
            try:
                key = {0: "空闲", 1: "有车", 2: "疑似僵尸车"}[int(status)]
            except (TypeError, ValueError, KeyError):
                key = "未知"
            stats[key] += 1
            details.append(f"{node['name']}: {key}")
        except Exception:
            stats["未知"] += 1
            details.append(f"{node['name']}: 查询失败")

    summary = f"共{len(cfg.get('nodes', []))}个车位: " + \
        ", ".join(f"{k}{v}个" for k, v in stats.items() if v > 0)
    return summary + "\n" + "\n".join(details)


@mcp.tool()
def query_plate(node_name: str) -> str:
    """查询指定车位的最新车牌识别结果(车牌号/置信度/颜色/摄像头状态/缩略图状态), node_name 如 Park001.
    车牌显示'-'时表示拍到了但没识别出来"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        status = client.query_device_status()
    except Exception as exc:
        return f"查询节点 {node_name} 失败: {exc}"
    if status != "在线":
        # 离线时属性快照是最后一次上报的旧识别结果, 不展示避免误导
        return f"节点 {node_name}: 状态={status} (设备离线, 不展示陈旧识别结果)"
    try:
        props = client.query_property()
    except Exception as exc:
        return f"节点 {node_name}: 在线但属性查询失败({exc})"

    plate = props.get("PlateNumber")
    plate_text = "未识别出(拍到但没认出)" if plate in (None, "", "-") else str(plate)
    bits = []
    conf = props.get("PlateConfidence")
    if conf not in (None, ""):
        bits.append(f"置信度={conf}%")
    color = props.get("PlateColor")
    if color not in (None, ""):
        try:
            bits.append(f"颜色={PLATE_COLOR_TEXT.get(int(color), color)}")
        except (TypeError, ValueError):
            bits.append(f"颜色={color}")
    cam = props.get("CameraOnline")
    if cam not in (None, ""):
        bits.append(f"摄像头={'在线' if cam in (True, 1, 'true', '1') else '离线'}")
    thumb = props.get("PlateThumb")
    bits.append(f"缩略图={'已上报(可用 get_plate_image 查看)' if thumb else '未上报'}")
    return f"节点 {node_name}: 车牌={plate_text}, " + ", ".join(bits)


@mcp.tool()
def get_plate_image(node_name: str) -> str:
    """返回指定车位最近一次车牌缩略图的 ASCII 点阵字符画(94×24), 供查看/核对拍摄内容.
    与 PlateNumber 是同一次识别的产物; 无图(未识别出牌/车已离场)时会提示未上报"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        status = client.query_device_status()
    except Exception as exc:
        return f"查询节点 {node_name} 失败: {exc}"
    if status != "在线":
        return f"节点 {node_name}: 状态={status} (设备离线, 不展示陈旧图像)"
    try:
        props = client.query_property()
    except Exception as exc:
        return f"节点 {node_name}: 在线但属性查询失败({exc})"
    thumb = props.get("PlateThumb")
    if not thumb:
        return f"节点 {node_name}: 车牌图像未上报(未识别出车牌或车已离场)"
    try:
        art = render_thumb_ascii(thumb)
    except Exception as exc:
        return f"节点 {node_name}: 车牌图像解码失败({exc})"
    plate = props.get("PlateNumber")
    plate_text = "未识别出(拍到但没认出)" if plate in (None, "", "-") else str(plate)
    return (f"节点 {node_name}: 车牌={plate_text} · 点阵图如下"
            f"(█=白字笔画, ·=黑底, 共94列×24行):\n{art}")


def _invoke_node_service(node_name, identifier, params, desc):
    """同步调用节点物模型服务(call-service), 返回面向 AI 的中文结果文本

    平台将命令经网关转发到节点并等待处理结果. 注意: 平台 code=0 只表示
    服务调用流程走完, 真实业务结果看输出参数 Result:
      - Result=1       设备收到并执行成功(ActualValue 为生效值)
      - Result=0       设备离线/下发超时/被拒, 应立即告知 AI 设备不可用
    """
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        resp = client.call_service(identifier, params)
    except Exception as exc:
        return f"{desc}节点 {node_name} 失败: {exc}"
    code = resp.get("code")
    if code != 0:
        return f"节点 {node_name} {desc}失败: code={code}, msg={resp.get('msg')}"
    output = resp.get("data") or {}
    if output.get("Result") == 1:
        out = ", ".join(f"{k}={v}" for k, v in output.items()) if output else ""
        return f"节点 {node_name} {desc}成功" + (f", 设备确认: {out}" if out else "")
    return (f"节点 {node_name} {desc}失败: 设备未确认(可能离线/超时)"
            f", Result={output.get('Result')}, ActualValue={output.get('ActualValue')}")


@mcp.tool()
def set_led_enable(node_name: str, enable: bool) -> str:
    """同步服务调用: 远程控制指定车位节点的 LED 使能开关, enable=True 开 / False 关, node_name 如 Park001"""
    return _invoke_node_service(node_name, "SetLed",
                                {"LedState": bool(enable)}, "设置LED使能")


@mcp.tool()
def set_zombie_threshold(node_name: str, threshold_sec: int) -> str:
    """同步服务调用: 设置指定车位的僵尸车判定阈值(秒), 范围 5~2592000, node_name 如 Park001"""
    return _invoke_node_service(node_name, "SetZombieThreshold",
                                {"ThresholdValue": int(threshold_sec)}, "设置僵尸车判定阈值")


@mcp.tool()
def set_sensor_distance(node_name: str, distance_cm: int) -> str:
    """同步服务调用: 设置指定车位的超声波判定距离阈值(cm), 范围 2~400, node_name 如 Park001"""
    return _invoke_node_service(node_name, "SetSensorDistance",
                                {"DistanceValue": int(distance_cm)}, "设置超声波判定距离阈值")


@mcp.tool()
def set_capture_policy(node_name: str, policy: int) -> str:
    """同步服务调用: 设置指定车位的拍照策略(0~3档): 0=不自动拍 1=有车即拍 2=僵尸车拍 3=都拍, node_name 如 Park001"""
    if int(policy) not in (0, 1, 2, 3):
        return f"拍照策略取值须为 0~3(0=不自动拍 1=有车即拍 2=僵尸车拍 3=都拍), 收到 {policy}"
    return _invoke_node_service(node_name, "SetCapturePolicy",
                                {"PolicyValue": int(policy)}, "设置拍照策略")


@mcp.tool()
def take_photo(node_name: str) -> str:
    """同步服务调用: 远程触发指定车位节点拍照识别(TriggerCapture): 摄像头拍照 -> 端侧识别 ->
    结果经 PlateNumber 属性异步上报. node_name 如 Park001. 识别出的车牌约2~5秒后用 query_plate 查询"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        resp = client.call_service("TriggerCapture", {})
    except Exception as exc:
        return f"触发节点 {node_name} 拍照失败: {exc}"
    code = resp.get("code")
    if code != 0:
        return f"节点 {node_name} 拍照失败: code={code}, msg={resp.get('msg')}"
    out = resp.get("data") or {}
    # 物模型语义: Result=1 识别出合法车牌; ActualValue=1 动作真的执行(0=节点忙/链路异常)
    if out.get("ActualValue") != 1:
        return (f"节点 {node_name} 拍照未执行: 节点忙或链路异常"
                f"(ActualValue={out.get('ActualValue')})")
    if out.get("Result") == 1:
        return f"节点 {node_name} 拍照并识别成功, 已出牌; 约2~5秒后用 query_plate 查看车牌"
    return f"节点 {node_name} 已执行拍照但未识别出车牌(将上报'-'); 稍后可用 query_plate 确认"


def main():
    parser = argparse.ArgumentParser(description="智能停车场 MCP 服务")
    parser.add_argument("--transport", choices=["stdio", "sse", "streamable-http"],
                        default="stdio",
                        help="传输方式: stdio(默认, MCP客户端本地启动) / sse / streamable-http(网络HTTP)")
    parser.add_argument("--host", default="0.0.0.0", help="网络模式监听地址")
    parser.add_argument("--port", type=int, default=8000, help="网络模式监听端口")
    args = parser.parse_args()

    if args.transport in ("sse", "streamable-http"):
        # 新版 mcp 的 run() 不接收 host/port, 通过 settings 配置
        mcp.settings.host = args.host
        mcp.settings.port = args.port
    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
