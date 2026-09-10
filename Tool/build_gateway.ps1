# =============================================================
# build_gateway.ps1 - One-click gateway build via arduino-cli
#
# Purpose: fix the S11 shared-header include path so that
#   <shared/lora_protocol.h> resolves (no manual -I needed).
#   - Arduino IDE users: no script needed (platform.local.txt
#     injects build.extra_flags; see Doc/ section 15.6)
#   - CLI users: run this script; include path auto-injected.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build_gateway.ps1
#   powershell -ExecutionPolicy Bypass -File .\build_gateway.ps1 -Fqbn esp8266:esp8266:generic
#   optional: $env:ARDUINO_CLI = "C:\path\to\arduino-cli.exe"
# =============================================================
param(
    [string]$Fqbn = "esp8266:esp8266:nodemcuv2"
)
$ErrorActionPreference = "Stop"

# Locate this script's directory (robust across -File / dot-source)
$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$Root   = Split-Path -Parent $ScriptDir   # parent of Tool/ = project root
$Sketch = Join-Path $Root "Gateway"
if (-not (Test-Path $Sketch)) { throw "Sketch dir not found: $Sketch" }

# --- locate arduino-cli: env var > Arduino IDE bundled > PATH ---
$Cli = $env:ARDUINO_CLI
if (-not $Cli) {
    $Candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
        (Join-Path $env:ProgramFiles        "Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe")
    )
    $Cli = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Cli) { $Cli = "arduino-cli" }

# --- inject include path: -I <project root> (forward slashes) ---
$IncludeFlag = "-I" + ($Root -replace "\\", "/")

Write-Host "==> arduino-cli : $Cli"
Write-Host "==> sketch      : $Sketch"
Write-Host "==> fqbn        : $Fqbn"
Write-Host "==> include     : $IncludeFlag"

& $Cli compile --fqbn $Fqbn --build-property "build.extra_flags=$IncludeFlag" $Sketch
exit $LASTEXITCODE
