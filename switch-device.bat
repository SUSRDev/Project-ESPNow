@echo off
chcp 65001 >nul
cd /d "%~dp0"
title ESPNow device switch (OLD / NEW ZLX)

if /I "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\switch-device.ps1" menu
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\switch-device.ps1" %*
)

echo.
pause
