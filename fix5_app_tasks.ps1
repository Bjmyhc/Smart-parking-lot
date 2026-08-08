$ErrorActionPreference = 'Stop'
$gbk = [System.Text.Encoding]::GetEncoding(936)
$path = "APP\app_tasks.c"
$txt = $gbk.GetString([System.IO.File]::ReadAllBytes($path))
$lines = New-Object System.Collections.ArrayList
$txt -split "`r?`n" | ForEach-Object { [void]$lines.Add($_) }

# 行号 -> 替换内容 (哈希表: 标量=单行, 数组=多行块, 行数保持不变)
$map = @{
# ---- 文件头注释 (2,4,5,6,8,9) ----
2 = ' * 应用层周期任务实现 - app_tasks.c'
4 = ' * 功能描述:'
5 = ' *   主循环中周期调用的任务函数实现(超声波/地磁/车位状态/OLED/WiFi)'
6 = ' *   数据上报与下行指令处理'
8 = ' * 作者: Bjmyhc'
9 = ' * 日期: 2026-07-25'
# ---- 宏定义分隔 (26) ----
26 = '/* ==================== 宏定义 ==================== */'
# ---- UPLOAD_INTERVAL 注释块 (28-35) ----
28 = @(
'#define UPLOAD_INTERVAL         15000  /* WiFi上报周期(ms)',
'                                         * 上传周期(15s): LoRa低带宽通道,',
'                                         * 发送(TX)耗时较长, 会占用链路,',
'                                         * 若频繁上报会造成拥堵, 3s间隔容易',
'                                         * 产生大量待发送数据帧!',
'                                         * 仅当StatusChanged被置位时',
'                                         * 可立即上传(状态变化触发) + 定时',
'                                         * occTimer每1s递增统计占用时长 */'
)
# ---- 单行宏定义 (36-48) ----
36 = '#define US_UPDATE_INTERVAL      100     /* 超声波采样周期(ms) */'
37 = '#define US_SHIFT                2       /* EMA滤波系数 alpha=1/4 */'
38 = '#define US_MIN_VALID            2       /* 超声波最小有效距离(cm) */'
39 = '#define US_MAX_VALID            400     /* 超声波最大有效距离(cm) */'
40 = '#define QMC_UPDATE_INTERVAL     100     /* 地磁采样周期(ms) */'
41 = '#define MAG_DEBOUNCE_CNT        2       /* 地磁消抖连续计数次数 */'
42 = '#define MAG_Z_SQ_THRESH         2.0f    /* 地磁Z轴磁场阈值 */'
43 = '#define PARK_CHECK_INTERVAL     200     /* 车位状态检测周期(ms) */'
44 = '#define DIST_THRESHOLD_CM       10      /* 超声波判断有车的距离阈值(cm) */'
45 = '#define OLED_UPDATE_INTERVAL    250     /* OLED刷新周期(ms) */'
46 = '#define WIFI_RECONNECT_DELAY   2000    /* 重连间隔(ms) */'
47 = '#define WIFI_MAX_RETRIES        3       /* 重连最大次数 */'
48 = '#define WIFI_RECONNECT_INTERVAL 30000   /* WiFi重连尝试最大间隔时间(ms) */'
# ---- HEARTBEAT_INTERVAL_MS 注释块 (50-53) ----
50 = @(
'#define HEARTBEAT_INTERVAL_MS   60000   /* MQTT发送PINGREQ心跳包间隔(ms)',
'                                         * 必须小于CONNECT keepalive(60s)时间,',
'                                         * 用于检测TCP链路, 特别LoRa链路',
'                                         * 因为信道较窄较慢(约256s超时) */'
)
# ---- LINK_DEAD_TIMEOUT_MS 注释块 (54-60): 超时改为 25000 (25s) ----
54 = @(
'#define LINK_DEAD_TIMEOUT_MS    25000   /* 链路失效判定时间(ms)',
'                                         * 小于心跳间隔(60s): 如果收到过',
'                                         * 任何消息(包括PINGRESP),',
'                                         * 判定链路正常, 不主动断开.',
'                                         * PINGRESP发送间隔(心跳帧0x00),',
'                                         * 故60s内必然更新lastRxTick, 25s',
'                                         * 无数据则判定PINGRESP丢失, 链路断开 */'
)
# ---- CONNECT_RESEND_TIMES 注释块 (62-70) ----
62 = @(
'#define CONNECT_RESEND_TIMES    3       /* 上线后重发次数',
'                                         * MQTT QoS0消息可能丢失: "Data',
'                                         * uploaded!"发送时可能尚未连上ESP8266',
'                                         * 透传, 造成消息丢失. 上线/重连成功后',
'                                         * 重发几次可确保LoRa信道中的数据帧被',
'                                         * 正确送达, 也能覆盖因OneNET的',
'                                         * 下行query指令造成的冲突帧丢失.',
'                                         * 每次3帧等间隔重发, 代价不大,',
'                                         * 确保上位机看到最新设备状态 */'
)
# ---- CONNECT_RESEND_INTERVAL 注释块 (71-74) ----
71 = @(
'#define CONNECT_RESEND_INTERVAL 1500    /* 重发间隔(ms)',
'                                         * 若SUBSCRIBE尚未订阅完成就重发, 需',
'                                         * TX发送占用的时间, 因此预留出TX时间',
'                                         * LoRa信道占用 */'
)
# ---- 全局变量分隔 (76) ----
76 = '/* ==================== 全局变量定义 ==================== */'
# ---- StatusChanged 注释块 (91-94) ----
91 = @(
'volatile uint8_t StatusChanged = 1;     /* 车位状态变化标志',
'                                         * 置位1: 有人/无人状态切换时',
'                                         * 触发立即上传, 不用等定时',
'                                         * 周期, 保证状态变化实时可见 */'
)
# ---- US_Task 函数头 (97-104) ----
97 = @(
' * 函数名: US_Task',
' * 功能:   超声波距离采样任务',
' * 参数:   无',
' * 返回:   无',
' * 说明:   有效距离过滤 + 滑动滤波 (EMA)',
' *         alpha = 1/4, 平滑距离采样波动, 避免抖动',
' *         超过/低于/读取失败均视为无效, 保留上次有效',
' *         滤波结果到 Distance'
)
# ---- US_Task 内部注释 (116,120) ----
116 = '        /* 有效距离过滤: 无效(=0), 超限, 失败 -> 保留上次有效值 */'
120 = '        /* 首次运行直接采用初始值 */'
# ---- QMC_Task 函数头 (139-145) ----
139 = @(
' * 函数名: QMC_Task',
' * 功能:   QMC5883P 地磁传感器任务',
' * 参数:   无',
' * 返回:   无',
' * 说明:   每100ms读取一次Z轴磁场强度',
' *         判定阈值: Z轴强度 > 阈值 判定有车',
' *         连续2次才更新状态, 避免瞬间磁场波动干扰'
)
# ---- QMC_Task 内部注释 (159,162,178) ----
159 = '            /* 判定阈值: Z轴磁场平方超过阈值 */'
162 = '            /* --- 消抖处理: 连续计数 --- */'
178 = '            /* 调试打印 */'
# ---- ParkingStatus_Check 函数头 (191-197) ----
191 = @(
' * 函数名: ParkingStatus_Check',
' * 功能:   车位状态检测',
' * 参数:   无',
' * 返回:   无',
' * 说明:   超声波与地磁联合判断车位占用状态',
' *         距离 < 10cm 且 地磁 触发 判定有车',
' *         连续占用超过设定时间判定为僵尸车位'
)
# ---- ParkingStatus_Check 内部注释 (212,215,222,225,230,233,242,245) ----
212 = '                    ParkStatus = PARK_OCCUPIED;         /* 状态: 有车 */'
215 = '                    StatusChanged = 1;                  /* 标记状态变化 */'
222 = '                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */'
225 = '                    StatusChanged = 1;                  /* 标记状态变化 */'
230 = '                    if (OccupiedTime > 5)               /* 僵尸判定: 5秒后进入 */'
233 = '                        StatusChanged = 1;              /* 标记状态变化 */'
242 = '                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */'
245 = '                    StatusChanged = 1;                  /* 标记状态变化 */'
# ---- LED_Task 函数头 (255-261) ----
255 = @(
' * 函数名: LED_Task',
' * 功能:   LED指示灯控制任务',
' * 参数:   无',
' * 返回:   无',
' * 说明:   使能时(LEDEnable=1), 根据车位状态控制LED亮灭',
' *         僵尸车位->常亮, 正常->熄灭',
' *         禁用时(LEDEnable=0), 强制熄灭LED'
)
# ---- OLED_Task 函数头 (279-284) ----
279 = @(
' * 函数名: OLED_Task',
' * 功能:   OLED屏幕显示任务',
' * 参数:   无',
' * 返回:   无',
' * 说明:   定时刷新, 每次更新OLED显示内容',
' *         显示WiFi连接状态与车位状态信息'
)
# ---- OLED 显示字符串 (293,295,297,298,299,303,306,309,312) ----
293 = '            OLED_ShowCH(0, 0, (u8 *)"WiFi已连接");'
295 = '            OLED_ShowCH(0, 0, (u8 *)"WiFi未连接");'
297 = '        OLED_Printf(0, 2, "地磁: %d", MagCarPresent);'
298 = '        OLED_Printf(0, 4, "距离: %.3d cm", Distance);'
299 = '        OLED_ShowCH(0, 6, (u8 *)"状态: ");'
303 = '                OLED_Printf(48, 6, "空闲     ");'
306 = '                OLED_Printf(48, 6, "有车 %3ds", OccupiedTime);'
309 = '                OLED_Printf(48, 6, "僵尸车位   ");'
312 = '                OLED_Printf(48, 6, "未知     ");'
# ---- GenerateParkingData 函数头 (321-332) ----
321 = @(
' * 函数名: GenerateParkingData',
' * 功能:   生成上报数据JSON格式',
' * 参数:   无',
' * 返回:   无',
' *',
' * 数据说明:',
' *   ParkStatus:    车位状态 (0=空闲, 1=有车, 2=僵尸占用)',
' *   GeoMagnetic:   地磁检测值',
' *   Ultrasonic:    超声波距离值(cm)',
' *   OccupiedTime:  占用时长(秒)',
' *   LED:           LED状态值 (true=亮起, false=熄灭, 只读展示)',
' *   LedEnable:     使能状态值 (true=开启, false=关闭, 可远程控制)'
)
# ---- Wifi_Reconnect 函数头 (354-361) ----
354 = @(
' * 函数名: Wifi_Reconnect',
' * 功能:   WiFi断线重连处理函数',
' * 参数:   无',
' * 返回:   无',
' * 说明:   检测到 WifiConnected=0 时调用',
' *         每次重试间隔逐渐增大, 避免频繁重试(影响正常通信)',
' *         超过30秒重试间隔上限, 会重置TCP(CIPCLOSE+CIPSTART)',
' *         重新建立MQTT CONNECT并保持Lora通信正常'
)
# ---- Wifi_Reconnect 内部注释 (369-370,379,388,392-393,396-398,405,410,422) ----
369 = @(
'    /* 首次调用时初始化时间基准, 间隔30秒后',
'     * 才会尝试Wifi_Init重新初始化ESP8266的TCP连接 */'
)
379 = '    /* 超过30秒重试间隔才执行 */'
388 = '        OLED_ShowCH(0, 0, (u8 *)"重连中...");'
392 = @(
'        /* 先退出透传模式, 发送AT指令(CIPCLOSE/CIPSTART等)',
'         * 确保ESP8266处于命令模式, 方便控制 */'
)
396 = @(
'        /* 本次重试需要重新初始化ESP8266(包括CIPCLOSE+CIPSTART+重新连接)',
'         * 然后MQTT CONNECT建立TCP连接, 保持',
'         * Init内部(ESP8266连接/断开连接)成功后调用DevLink, 并订阅数据 */'
)
405 = '        /* 等待Lora数据缓冲/TCP缓冲清空后再连接MQTT */'
410 = '            OLED_ShowCH(0, 0, (u8 *)"连接成功!");'
422 = '    OLED_ShowCH(0, 0, (u8 *)"连接失败!");'
# ---- Wifi_Task 函数头 (429-446) ----
429 = @(
' * 函数名: Wifi_Task',
' * 功能:   WiFi通信与数据上报任务',
' * 参数:   无',
' * 返回:   无',
' * 说明:   定时(15s)上报传感器数据到OneNET平台',
' *         断线时进行自动重连(最多3次)',
' *',
' *         结合Lora链路本身的特性(低速信道):',
' *         1. 定时上传(15s): LoRa使节点设备占据信道(TX)时',
' *            耗时较长, 应避免频繁上传造成链路拥塞. 状态变化(3s)',
' *            内可立即上传, 覆盖实时性需求.',
' *         2. 节点设备上传时LoRa信道处于RX状态, 下行指令到达',
' *            会先进入节点设备的缓冲区, 无法立即上传',
' *            ESP8266_GetIPD解析下行指令, 可能造成部分丢失.',
' *         3. 每条上行数据到达平台后都会触发下行, 包括ESP8266_Clear',
' *            等操作(对应平台下行ack/命令响应).',
' *         4. 上行发送期间请勿执行下行操作, 避免Delay阻塞等',
' *            OLED/显示刷新操作.'
)
# ---- Wifi_Task 内部注释 (452-454,456-461,471,478-481,490-492,505-507,511,514-517,528,538,542-546) ----
452 = '    static uint8_t  wasConnected = 0;       /* 记录上次连接状态(用于上升沿检测) */'
453 = '    static uint8_t  resendRemain = 0;       /* 剩余待重发帧数 */'
454 = '    static uint32_t lastResendTick = 0;     /* 上次重发时间戳(ms) */'
456 = @(
'    /* === 上线后立即重发 ===',
'     * 刚"连上"(上升沿: 断开->连接成功), 立即重发:',
'     * MQTT QoS0消息可能丢失, 上线/重连成功后仍可能丢在LoRa',
'     * 上行链路/平台缓冲中, 主动重发几次, 确保',
'     * query指令后设备状态更新. 每次3帧(间隔1.5s)依次重发',
'     * 减少碰撞, 保证上位机看到最新设备状态 */'
)
471 = '    /* 未连接时执行重连逻辑, 并跳过本轮其他操作 */'
478 = @(
'    /* === 链路活性检测(心跳超时) ===',
'     * 超过阈值未收到任何下行消息(包括PINGRESP) 视为 链路断开,',
'     * 主动断开重连, 避免长时间假在线(收不到任何响应,',
'     * TCP连接已断但设备未感知, 不主动断开, 一直假在线) */'
)
490 = @(
'    /* === MQTT心跳(PINGREQ) ===',
'     * 每60s发送一次, 并等待PINGRESP(2字节响应包), 维持TCP连接,',
'     * 并更新链路活性: PINGRESP到达后会刷新lastRxTick */'
)
505 = @(
'    /* 上报条件判断: 定时到 或 状态变化(如车位占用变化) 或 重发中',
'     * 说明: 状态变化(车位/地磁变化)>0 或 剩余重发帧数>=阈值,',
'     * 则 1.5s 间隔连续重发最多3次, 应对QoS0消息可能丢失的情况 */'
)
511 = '        /* 清除状态变化标记, 避免重复上报 */'
514 = @(
'        /* 上报前先处理完下行指令缓存, 确保没有残留的LoRa帧.',
'         * 处理逻辑: OneNet_RevPro 每帧解析并做业务处理, 可处理多帧',
'         * 直到缓存为空. 处理过程中可能触发状态位变化(如LED/α设置)',
'         * 部分会等待下次上报周期再处理 */'
)
528 = '            /* 重发流程: 递减计数, 更新时间戳以保持间隔 */'
538 = '        /* 本次已上传, 避免本轮剩余逻辑重复执行(直接返回) */'
542 = @(
'    /* === 下行指令处理(无上报时, 轮询处理) ===',
'     * LoRa链路: 节点设备长时间占用信道RX, 下行指令到达',
'     * ESP8266缓冲区, 必须及时读取, 否则可能丢失. 这里在',
'     * 非上报周期内轮询: RevPro 逐条解析并立即响应, 且同时处理',
'     * 多条(无残留处理则清空缓存) */'
)
}

foreach ($start in ($map.Keys | Sort-Object)) {
    $v = $map[$start]
    $idx = $start - 1
    if ($idx -lt 0 -or $idx -ge $lines.Count) { throw "行号越界: $start" }
    if ($v -is [string]) {
        $lines[$idx] = $v
    } else {
        if (($idx + $v.Count) -gt $lines.Count) { throw "超出范围: 行 $start 需要 $($v.Count) 行" }
        for ($k = 0; $k -lt $v.Count; $k++) {
            $lines[$idx + $k] = $v[$k]
        }
    }
}

$out = $lines -join "`r`n"
[System.IO.File]::WriteAllBytes($path, $gbk.GetBytes($out))
Write-Output "app_tasks.c 注释修复完成, 共替换 $($map.Count) 个块"
