<#
=====================================================================
 flash_esp32.ps1 - 一键烧录 ESP32-S3 摄像头固件（替代 VSCode 底部的烧录按钮）

 VSCode 里那个按钮本质就是跑：idf.py -p <COM> flash
 这里做的事完全一样，只是把"先激活 IDF 环境"这一步也包进去了。

 用法：
   # 1) 最常用：编译 + 烧录
   powershell -ExecutionPolicy Bypass -File .\Tool\flash_esp32.ps1

   # 2) 指定串口
   .\Tool\flash_esp32.ps1 -Port COM19

   # 3) 只烧不编译（刚 build 过，bin 没变）
   .\Tool\flash_esp32.ps1 -NoBuild

   # 4) 烧完自动读 8 秒开机日志（确认版本号变成新版本）
   .\Tool\flash_esp32.ps1 -Monitor 8

   # 5) 全清重建（改了 sdkconfig / 换了 IDF 版本后必须来一次）
   .\Tool\flash_esp32.ps1 -FullClean

 参数说明：
   -Port       串口名，默认自动探测（找到唯一的 CH340/CP210x 就用它）
   -Baud       烧录波特率，默认 460800
   -NoBuild    跳过 idf.py build
   -FullClean  先 idf.py fullclean 再 build
   -Monitor N  烧完读 N 秒日志（走 Tool/esp_serial.ps1，不是交互式 TUI）

 前置条件（本机已满足，记录在此备查）：
   - ESP-IDF v5.5.6 装在 D:\Data\Documents\Esp\.espressif\v5.5.6\esp-idf
   - 环境激活脚本 C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1
   - 工程目录 PlateRecognitionIDF\，产物 build\plate_recognition.bin
=====================================================================
#>
param(
    [string]$Port = "",
    [int]$Baud = 460800,
    [switch]$NoBuild,
    [switch]$FullClean,
    [int]$Monitor = 0
)

$ErrorActionPreference = "Stop"

# ---- 定位工程根（脚本在 Tool\ 下，根在上一级）----
$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$Root    = Split-Path -Parent $ScriptDir
$Project = Join-Path $Root "PlateRecognitionIDF"
if (-not (Test-Path $Project)) { throw "找不到工程目录: $Project" }

# ---- 自动探测串口 ----
if (-not $Port) {
    $cands = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($cands.Count -eq 0) { throw "没有检测到任何串口，检查 USB 线 / 驱动（CH340）" }
    if ($cands.Count -gt 1) {
        Write-Host "检测到多个串口: $($cands -join ', ')"
        Write-Host "请用 -Port 指定，例如 -Port $($cands[0])"
        throw "串口不唯一"
    }
    $Port = $cands[0]
    Write-Host "自动选中串口: $Port"
}

# ---- 激活 ESP-IDF 环境 ----
$profilePs1 = "C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1"
if (-not (Test-Path $profilePs1)) {
    throw "找不到 IDF 环境脚本: $profilePs1（IDF 版本换过的话改这个路径）"
}
& $profilePs1 *> $null
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "idf.py 不可用 —— IDF 环境没激活成功"
}

Push-Location $Project
try {
    if ($FullClean) {
        Write-Host "==> idf.py fullclean"
        idf.py fullclean
        if ($LASTEXITCODE -ne 0) { throw "fullclean 失败 (exit $LASTEXITCODE)" }
    }

    if (-not $NoBuild) {
        Write-Host "==> idf.py build"
        idf.py build
        if ($LASTEXITCODE -ne 0) { throw "build 失败 (exit $LASTEXITCODE)" }
    }

    $bin = Join-Path $Project "build\plate_recognition.bin"
    if (-not (Test-Path $bin)) { throw "找不到产物: $bin" }
    $size = (Get-Item $bin).Length
    Write-Host ("==> 待烧录: plate_recognition.bin {0} 字节 ({1:N0} KB)" -f $size, ($size / 1KB))

    Write-Host "==> idf.py -p $Port -b $Baud flash"
    idf.py -p $Port -b $Baud flash
    if ($LASTEXITCODE -ne 0) { throw "烧录失败 (exit $LASTEXITCODE)" }

    Write-Host "==> 烧录完成"

    if ($Monitor -gt 0) {
        Write-Host "==> 读 $Monitor 秒开机日志"
        $serial = Join-Path $Root "Tool\esp_serial.ps1"
        powershell -ExecutionPolicy Bypass -File $serial -Port $Port -Reset -Seconds $Monitor
    }
}
finally {
    Pop-Location
}
