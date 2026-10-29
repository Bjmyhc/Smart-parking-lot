# =============================================================
# fix_ide_include.ps1 - 项目目录搬家后, 一键修好 Arduino IDE 的编译
#
# 背景:
#   Gateway/lora_protocol.h 用 <shared/lora_protocol.h> 引共享协议头,
#   依赖 -I <项目根>。arduino-cli 由 Tool/build_gateway.ps1 现算现注入,
#   而 Arduino IDE 只能靠 esp8266 核心里的 platform.local.txt 注入,
#   那里面是**绝对路径** —— 项目目录一搬(或换台电脑), IDE 就报:
#       fatal error: shared/lora_protocol.h: No such file or directory
#
# 本脚本做的事:
#   1. 找到本机 esp8266 核心目录(可有多个版本)
#   2. 用本脚本所在位置反推项目根, 算出正确的 -I 路径
#   3. 覆写 platform.local.txt(UTF-8 无 BOM), 打印结果
#
# 用法(项目搬家后跑一次, 或 IDE 报上面那个错时跑一次):
#   powershell -ExecutionPolicy Bypass -File .\fix_ide_include.ps1
#   (只打印不写盘: 加 -DryRun)
# =============================================================
param(
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$Root = Split-Path -Parent $ScriptDir          # Tool/ 的父目录 = 项目根
$Shared = Join-Path $Root "shared\lora_protocol.h"
if (-not (Test-Path $Shared)) {
    throw "没找到 $Shared —— 本脚本必须放在项目的 Tool\ 目录下运行"
}

# 正斜杠 + 不结尾斜杠, 与 arduino 的 build.extra_flags 习惯一致
$IncludePath = $Root -replace "\\", "/"
$Line = "build.extra_flags=-I$IncludePath"

$CoreRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp8266\hardware\esp8266"
if (-not (Test-Path $CoreRoot)) {
    throw "没找到 esp8266 核心目录: $CoreRoot (先在 Arduino IDE 里装好 esp8266 开发板支持)"
}
$Cores = Get-ChildItem $CoreRoot -Directory | Where-Object { Test-Path (Join-Path $_.FullName "platform.txt") }
if (-not $Cores) { throw "在 $CoreRoot 下没找到任何 esp8266 核心版本" }

Write-Host "==> 项目根 : $Root"
Write-Host "==> -I     : $IncludePath"
Write-Host ""

foreach ($core in $Cores) {
    $file = Join-Path $core.FullName "platform.local.txt"
    $body = @"
# ============================================================
# platform.local.txt - 本机用户级核心配置(勿提交仓库)
#
# 由 Tool/fix_ide_include.ps1 生成, 项目目录搬家后重新跑一次即可。
# 作用: Arduino IDE 编译网关(Gateway/)时注入项目根 include 路径,
#       使 Gateway/lora_protocol.h 里的 <shared/lora_protocol.h> 可解析。
# 原理: esp8266 核心 recipe.c/cpp.o.pattern 均引用 {build.extra_flags},
#       本文件覆盖 build.extra_flags 即同时作用于 C/C++ 编译。
#       (该核心的 build.extra_flags 默认为空, 覆盖不会丢任何默认宏)
# ============================================================
$Line
"@
    if ($DryRun) {
        Write-Host "[DryRun] 会写: $file"
        Write-Host $body
        continue
    }
    # 必须 UTF-8 无 BOM: 带 BOM 时第一行属性名会被污染, IDE 读不出该属性
    [System.IO.File]::WriteAllText($file, $body, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[OK] 已写入: $file"
    Write-Host "     $Line"
}

Write-Host ""
Write-Host "现在回到 Arduino IDE 直接点【验证/编译】即可; arduino-cli 用户可继续用 Tool/build_gateway.ps1。"
