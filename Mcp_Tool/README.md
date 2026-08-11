# 智能停车场 MCP 工具

让 **小智 AI 语音助手**（xiaozhi-esp32 生态）通过 MCP（Model Context Protocol）实时查询停车场状态、远程控制设备。

## 数据链路

```
STM32节点 → LoRa → ESP8266网关 → OneNET云平台 → 本MCP工具 → 小智AI
```

工具直接调用 **OneNET 云平台 HTTP API**（与 `web前端/app.js` 同一套接口），**网关和节点代码无需任何改动**。

## 文件说明

| 文件 | 说明 |
|------|------|
| `parking_mcp_server.py` | MCP 服务主程序（5 个工具） |
| `onenet_client.py` | OneNET HTTP API 封装（查询/设置属性） |
| `config.example.json` | 配置示例（复制为 `config.json` 后填写） |
| `config.json` | **实际配置**（含设备 token，已加入 .gitignore 不入库） |
| `requirements.txt` | Python 依赖 |

## 快速开始

### 1. 安装依赖（需 Python 3.10+）

```bash
pip install -r requirements.txt
```

### 2. 配置设备信息

```bash
copy config.example.json config.json
```

编辑 `config.json`，每个节点需要三样东西：

- `product_id`：子设备产品 ID（当前为 `04kjwU9TC7`，见 `Node/BSP/lora_node.h`）
- `device_name`：子设备设备名（节点1=`Park001`，节点2=`Park002`）
- `token`：该设备的鉴权 token

> 注意：配置文件里已填好最新的产品 ID 和设备名（旧的 `53BV12EYcY/park1` 已废弃），只需补上 token。

### 3. 获取设备 token

登录 [OneNET 控制台](https://open.iot.10086.cn)，进入 **设备详情 → 鉴权信息/APIKey**，为每个子设备（Park001、Park002）各生成一个 token。

token 格式形如：
`version=2018-10-31&res=products%2F<产品ID>%2Fdevices%2F<设备名>&et=...&method=md5&sign=...`

> 重要：子设备 token 代码里没有（网关代上线用的是网关自己的 token），必须自己在控制台生成。

> 每个设备一个 token，节点多了就在 `nodes` 数组里加一条。

### 4. 运行

**stdio 模式**（MCP 客户端本地启动，推荐给小智服务端用）：

```bash
python parking_mcp_server.py
```

**SSE 模式**（HTTP 网络访问，适合小智服务端远程连接）：

```bash
python parking_mcp_server.py --transport sse --port 8000
```

启动后可用任意 MCP 客户端（Claude Desktop、Cursor、Cherry Studio 等）连接验证。

## MCP 工具列表

| 工具 | 功能 | 示例问法 |
|------|------|----------|
| `list_nodes` | 列出所有已配置节点 | "有哪些车位？" |
| `query_node_status` | 查询单个节点实时状态 | "Park001 现在什么情况？" |
| `query_all_nodes` | 查询所有节点实时状态 | "所有车位都查一遍" |
| `get_parking_summary` | 停车场概览（空闲/有车/僵尸车统计） | "停车场现在空几个位？" |
| `set_led_enable` | 远程控制某节点 LED 使能 | "把 Park001 的灯关掉" |

## 接入小智 AI（xiaozhi-esp32）

小智服务端（`xiaozhi-esp32-server` 各版本）都支持挂载 MCP 工具，两种接入方式：

### 方式一：SSE 模式（推荐，工具可远程调用）

1. 在电脑/服务器上启动 SSE 模式：
   ```bash
   python parking_mcp_server.py --transport sse --port 8000
   ```
2. 在小智服务端配置 MCP 服务器地址为：`http://<本机IP>:8000/sse`
   - Python 版小智服务端：在 `config.yaml` 的 mcp 配置段，或智控台"智能体 → 配置角色"中启用 MCP
   - 部分版本通过 `mcp-proxy` 桥接：`mcp-proxy http://<本机IP>:8000/sse`

### 方式二：stdio 模式（本机部署小智服务端时）

在小智服务端配置 mcpServers：

```json
"parking": {
  "command": "python",
  "args": ["<本机路径>/parking_mcp_server.py"],
  "cwd": "<本机路径>"
}
```

配置后重启小智服务端，语音对话时即可调用工具，例如：

> "小智，停车场现在还有几个空位？" → 调 `get_parking_summary`
> "小智，把 1 号车位的灯关了" → 调 `set_led_enable`

## 注意事项

- 云端数据是网关每 15s 上报一次的，查询结果最多有 15s 延迟（对语音查询足够）
- 控制命令走 OneNET 平台下行 → 网关 MQTT → LoRa → 节点执行，整个链路约 1~3s
- `config.json` 含设备 token，**不要提交到 git**（已在 `.gitignore` 中忽略）
