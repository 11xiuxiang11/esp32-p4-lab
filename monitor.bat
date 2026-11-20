@echo off
rem  monitor.bat -- 双击即可看串口输出（COM3，115200）
rem  前提：板子已插好、固件已烧录。按 Ctrl+] 退出。
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" -p COM3 monitor
pause
