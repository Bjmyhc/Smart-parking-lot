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
  get_parking_summary()   停车场概览(空闲/有车/僵尸车统计)
  set_led_enable()        远程控制某节点 LED 使能开关

运行方式:
  stdio(默认):   python parking_mcp_server.py
  sse(网络):     python parking_mcp_server.py --transport sse --port 8000

配置文件: config.json (由 config.example.json 复制并填写各设备 token 生成)
"""

import argparse
import json
import os

from mcp.server.fastmcp import FastMCP
from onenet_client import OneNetClient

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
    """查询指定车位节点的实时状态(停车状态/超声波距离/地磁/占用时长/LED), node_name 如 Park001"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        props = client.query_property()
    except Exception as exc:  # 网络/平台异常
        return f"查询节点 {node_name} 失败: {exc}"
    return f"节点 {node_name}: {OneNetClient.format_props(props)}"


@mcp.tool()
def query_all_nodes() -> str:
    """查询所有已配置车位节点的实时状态"""
    cfg = load_config()
    parts = []
    for node in cfg.get("nodes", []):
        client = OneNetClient(
            node["product_id"], node["device_name"], node["token"],
            timeout=cfg.get("query_timeout", 10),
        )
        try:
            props = client.query_property()
            parts.append(f"{node['name']}: {OneNetClient.format_props(props)}")
        except Exception as exc:
            parts.append(f"{node['name']}: 查询失败({exc})")
    return "\n".join(parts) if parts else "未配置任何节点"


@mcp.tool()
def get_parking_summary() -> str:
    """停车场概览: 统计各车位占用情况(空闲/有车/僵尸车), 供快速回答'停车场现在什么情况'"""
    cfg = load_config()
    stats = {"空闲": 0, "有车": 0, "疑似僵尸车": 0, "未知": 0}
    details = []
    for node in cfg.get("nodes", []):
        client = OneNetClient(
            node["product_id"], node["device_name"], node["token"],
            timeout=cfg.get("query_timeout", 10),
        )
        try:
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
def set_led_enable(node_name: str, enable: bool) -> str:
    """远程控制指定车位节点的 LED 使能开关, enable=True 开 / False 关, node_name 如 Park001"""
    cfg = load_config()
    client = get_client(cfg, node_name)
    try:
        resp = client.set_property({"LedEnable": bool(enable)})
    except Exception as exc:
        return f"设置节点 {node_name} LED 使能失败: {exc}"

    code = resp.get("code")
    if code in (200, 0):
        action = "开启" if enable else "关闭"
        return f"节点 {node_name} LED 已{action}(命令已下发, 设备确认)"
    return f"节点 {node_name} LED 设置失败: code={code}, msg={resp.get('msg')}"


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
