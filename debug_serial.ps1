# debug_serial.ps1 —— 串口 GDB 调试（不依赖 JTAG）
# 流程：向 COM 口发 0x03（Ctrl+C）让固件进入 GDB Stub → 用 gdb 连上
# 用法： .\debug_serial.bat [COM3]

param(
    [string] $Port = "COM3"
)

$py  = "E:\idf\tools\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$gdb = "E:\idf\tools\tools\riscv32-esp-elf-gdb\16.3_20250913\riscv32-esp-elf-gdb\bin\riscv32-esp-elf-gdb.exe"
$elf = Join-Path $PSScriptRoot "build\p4_lab.elf"

if (-not (Test-Path $elf)) {
    Write-Host "找不到 $elf，先跑 idf.bat build" -ForegroundColor Red
    exit 1
}

# 1) 发 0x03 触发运行时 GDB Stub，然后立刻释放串口
& $py -c "import serial,time,sys; s=serial.Serial(sys.argv[1],115200,timeout=0.2); s.write(b'\x03'); s.flush(); time.sleep(0.3); s.close(); print('已发送 0x03，固件应进入 GDB Stub')" $Port
if ($LASTEXITCODE -ne 0) {
    Write-Host "打开 $Port 失败：确认板子已连接、且没有 monitor 占着串口" -ForegroundColor Red
    exit 1
}

# 2) gdb 连上去，进入交互
& $gdb $elf -ex "set serial baud 115200" -ex "target remote $Port"
