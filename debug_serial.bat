@echo off
rem 串口 GDB 调试入口：debug_serial.bat COM3
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0debug_serial.ps1" %*
pause
