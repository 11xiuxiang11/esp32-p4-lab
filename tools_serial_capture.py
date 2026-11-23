"""临时脚本：把 COM3 串口输出存成文件（后台跑，供 http 测试取证）"""
import serial
import sys
import time

port = sys.argv[1]
out_path = sys.argv[2]
duration_s = float(sys.argv[3])

ser = serial.Serial(port, 115200, timeout=0.2)
out = open(out_path, "w", encoding="utf-8", errors="replace")
end = time.time() + duration_s
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            out.write(data.decode("utf-8", errors="replace"))
            out.flush()
finally:
    ser.close()
    out.close()
