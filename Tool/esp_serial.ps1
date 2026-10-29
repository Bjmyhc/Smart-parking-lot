<#
=====================================================================
 esp_serial.ps1 - ESP32-S3 摄像头固件的串口工具（替代 VSCode 的串口监视器）

 为什么需要它：
   `idf.py monitor` 是交互式 TUI，在 DSH/脚本里跑会卡住、也拿不到干净的日志。
   这个脚本做三件事：读日志 / 发 AT 命令 / 软复位，全部非交互、可直接在 DSH 里跑。

 用法（PowerShell）：
   # 1) 读 5 秒日志（默认 COM19 @921600）
   powershell -ExecutionPolicy Bypass -File .\Tool\esp_serial.ps1

   # 2) 读 15 秒，存到文件
   .\Tool\esp_serial.ps1 -Seconds 15 -OutFile log.txt

   # 3) 发命令（自动先抢控制权 AT+CTRL=PC，再发，最后还回去）
   .\Tool\esp_serial.ps1 -Send "AT+THUMB?" -Seconds 2
   .\Tool\esp_serial.ps1 -Send "AT+CTRL=PC;AT+THUMB?;AT+INFO"   # 多条用 ; 分隔

   # 4) 只软复位（RTS 脉冲 = 按一下 RST），然后读 6 秒开机横幅
   .\Tool\esp_serial.ps1 -Reset -Seconds 6

 注意：
   - 电脑口（USB 控制台）波特率是 921600（见 Doc/AT指令手册.md）；
     烧录用的是 460800（esptool 自动切），两者不同，别混。
   - P5.80 起节点口走 UART1（GPIO47/48），$PLATE 会 [镜像] 到电脑口，
     所以不接节点板也能在这里看到识别结果。
   - 正常模式下控制权在节点手里，电脑口发 AT+XXX 会回「没有权限」，
     脚本用 -Send 时会自动 AT+CTRL=PC → 发命令 → AT+CTRL=NODE。
=====================================================================
#>
param(
    [string]$Port = "COM19",
    [int]$Baud = 921600,
    [int]$Seconds = 5,
    [string]$Send = "",            # 多条命令用 ; 分隔，例如 "AT+CTRL=PC;AT+THUMB?"
    [switch]$Reset,
    [switch]$KeepControl,          # 发完不把控制权还给节点（调试时方便连续发）
    [string]$OutFile
)

$ErrorActionPreference = "Stop"

function Read-Available {
    param($sp, [int]$waitMs)
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddMilliseconds($waitMs)
    while ((Get-Date) -lt $deadline) {
        if ($sp.BytesToRead -gt 0) {
            $n = $sp.BytesToRead
            $buf = New-Object byte[] $n
            $sp.Read($buf, 0, $n) | Out-Null
            [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buf))
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
    return $sb.ToString()
}

function Send-Line {
    param($sp, [string]$text)
    $b = [System.Text.Encoding]::ASCII.GetBytes($text + "`r`n")
    $sp.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 120
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
try {
    $sp.Open()
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.DiscardInBuffer()

    if ($Reset) {
        # RTS 低脉冲 = 拉低 EN 脚 = 复位（ESP32-S3 开发板的标准自动复位接法）
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds 150
        $sp.RtsEnable = $false
        Start-Sleep -Milliseconds 300
        $sp.DiscardInBuffer()
    }

    $out = New-Object System.Text.StringBuilder

    if ($Send -ne "") {
        $cmds = $Send -split ';' | Where-Object { $_ -ne '' }
        # 抢控制权（已在 PC 手里时这条也会回 OK，幂等）
        Send-Line $sp "AT+CTRL=PC"
        [void]$out.Append((Read-Available $sp 300))

        foreach ($cmd in $cmds) {
            Send-Line $sp $cmd
            [void]$out.Append(">>> $cmd`r`n")
            [void]$out.Append((Read-Available $sp 700))
        }

        if (-not $KeepControl) {
            Send-Line $sp "AT+CTRL=NODE"
            [void]$out.Append((Read-Available $sp 300))
        }
    } else {
        [void]$out.Append((Read-Available $sp ($Seconds * 1000)))
    }

    $text = $out.ToString()

    if ($OutFile) {
        [System.IO.File]::WriteAllText((Join-Path (Get-Location) $OutFile), $text,
            (New-Object System.Text.UTF8Encoding $false))
        Write-Host "已写入 $OutFile（$($text.Length) 字符）"
    }
    Write-Output $text
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
