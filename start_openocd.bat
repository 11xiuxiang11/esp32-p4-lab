@echo off
rem ============================================================
rem  start_openocd.bat -- 启动 OpenOCD（JTAG 调试的"服务器"）
rem
rem  用法：双击，或在终端里运行。窗口不要关，它是 gdb 的服务器。
rem  启动成功后会看到：
rem      JTAG tap: esp32p4.tap0 ... found
rem      [hp.cpu0] Examination succeed
rem      Listening on port 3333 for gdb connections
rem  然后回 VS Code 按 F5，或者在另一个终端跑 gdb。
rem  Ctrl+C 关闭。
rem ============================================================
setlocal

set "OCD=E:\idf\tools\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin\openocd.exe"
set "SCRIPTS=E:\idf\tools\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts"

"%OCD%" -s "%SCRIPTS%" -f board/esp32p4-builtin.cfg

endlocal
