@echo off
chcp 65001 >nul
cd /d %~dp0
echo ==========================================
echo  One-key OTA firmware pack
echo  input : ..\Node\Output\app.bin
echo  output: ..\Node\Output\node_v{ver}.bin
echo ==========================================
echo.
python fw_pack.py
if %errorlevel% neq 0 (
    echo.
    echo [FAIL] pack failed, see messages above.
    pause
    exit /b 1
)
echo.
echo [DONE] ready: ..\Node\Output\app_fw.bin
pause
