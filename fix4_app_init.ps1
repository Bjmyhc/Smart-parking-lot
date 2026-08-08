$ErrorActionPreference = 'Stop'
$gbk = [System.Text.Encoding]::GetEncoding(936)
$path = "APP\app_init.c"
$txt = $gbk.GetString([System.IO.File]::ReadAllBytes($path))
$lines = New-Object System.Collections.ArrayList
$txt -split "`r?`n" | ForEach-Object { [void]$lines.Add($_) }

$map = @{
1 = @(
'/****************************************************************************',
' * 应用层初始化函数实现 - app_init.c',
' *',
' * 功能描述:',
' *   系统启动时统一执行的初始化流程, 封装硬件和业务初始化',
' *   包括 BSP 外设初始化和 WiFi 连接初始化(最多3次重试)',
' *',
' * 作者: Bjmyhc',
' * 日期: 2026-07-25',
' ****************************************************************************/'
)
24 = @('#define WIFI_INIT_MAX_RETRIES    3       /* WiFi初始化最大重试次数 */')
25 = @('#define WIFI_INIT_RETRY_DELAY    1000    /* 重试间隔(ms) */')
27 = @(
'/****************************************************************************',
' * 函数名: BSP_Init',
' * 功能:   初始化所有板级支持包(BSP)设备',
' * 参数:   无',
' * 返回值: 无',
' ****************************************************************************/'
)
44 = @(
'/****************************************************************************',
' * 函数名: Wifi_Init',
' * 功能:   WiFi模块初始化并连接OneNET平台',
' * 参数:   无',
' * 返回值: 无',
' * 说明:   流程: ESP8266初始化 -> 连接平台(最多3次重试)',
' *         每次重试OLED屏幕显示当前状态',
' *         连接成功后订阅主题并设置WifiConnected标志',
' ****************************************************************************/'
)
60 = @('        OLED_Printf(0, 0, "WiFi初始化...");')
62 = @('            OLED_Printf(0, 2, "重试(%d/3)", i);')
65 = @(
'        /* 每次尝试都重新初始化ESP8266(含CIPCLOSE+CIPSTART重建TCP)',
'         * MQTT协议规定同一TCP连接只能发一个CONNECT,',
'         * 失败后需要重建TCP再连接',
'         * Init失败(ESP8266无响应)则跳过DevLink, 直接进入下一轮重试 */'
)
76 = @('        OLED_ShowCH(0, 0, (u8 *)"正在连接平台");')
79 = @(
'        /* 等待Lora模块网络/TCP稳定后再发起MQTT连接,',
'         * 确保CONNACK能在OneNet_DevLink内部超时内收到 */'
)
86 = @('            OLED_ShowCH(0, 0, (u8 *)"连接成功!");')
98 = @('    /* 3次重试均失败 */')
100 = @('    OLED_ShowCH(0, 0, (u8 *)"连接失败!");')
}

foreach ($start in ($map.Keys | Sort-Object)) {
    $newLines = $map[$start]
    $idx = $start - 1
    if (($idx + $newLines.Count) -gt $lines.Count) { throw "超出范围: 行 $start 需要 $($newLines.Count) 行" }
    for ($k = 0; $k -lt $newLines.Count; $k++) {
        $lines[$idx + $k] = $newLines[$k]
    }
}

$out = $lines -join "`r`n"
[System.IO.File]::WriteAllBytes($path, $gbk.GetBytes($out))
Write-Output "app_init.c 注释修复完成, 共替换 $($map.Count) 个块"
