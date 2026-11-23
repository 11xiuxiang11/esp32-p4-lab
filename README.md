# ESP32-P4 双芯网络实验（p4-lab）

基于 **ESP32-P4 + ESP32-C6** 的双芯片实验工程：P4 通过 SDIO 把 C6 当网卡用（esp-hosted / esp_wifi_remote），在它上面跑 FreeRTOS 与 lwIP，验证从 Wi-Fi 关联、DHCP、DNS、TCP 到断线重连的完整链路。

## 亮点

- **双芯网络**：P4↔C6 走 SDIO 4-bit / 40 MHz，hosted RPC；实测 DHCP → TCP → 回显全链路。
- **lwIP 客户端**：非阻塞 connect + 心跳 + 指数退避（带抖动）重连；断开检测分「主动断开 20 ms」和「静默丢失 15.1 s」两条路径。
- **显示与相机**：JD9165 MIPI-DSI 1024×600 点屏 + LVGL 9 界面 + GT911 触摸；SC2336 采集（30 fps）直绘叠加，相机 + UI 同屏 29~30 fps。
- **网络图传**：相机 → 硬件 JPEG（7~8 ms/帧）→ `esp_http_server`，浏览器看 `/jpg` 单帧和 `/stream` MJPEG 连续流（11~14 fps）。
- **FreeRTOS 全量实验**：任务/优先级/双核、竞争与互斥量、队列、信号量、事件组、中断、内存、四类故障（栈溢出/堆耗尽/看门狗/死锁）。
- **可复现**：编译期实验选择器、0 warning、关键数据都有日志。

## 硬件与软件

| 项 | 值 |
|---|---|
| 板卡 | ESP32-P4X-Function-EV-Board（P4 rev3.1 + C6） |
| 主控 | ESP32-P4（RISC-V 双核） |
| 网卡 | ESP32-C6，SDIO 4-bit，40 MHz |
| 显示屏 | JD9165BA，MIPI-DSI 2-lane 600 Mbps，1024×600 |
| 触摸 | GT911（I2C1，与相机 SCCB 共用） |
| 相机 | SC2336，MIPI-CSI，RAW8 → ISP → RGB565 |
| IDF | v5.5.3 |
| 组件 | esp_hosted 2.12.9、esp_wifi_remote 1.3.0、esp_video 2.6.0、LVGL 9.6 |
| 控制台 | USB-Serial/JTAG |

## 数据通路

```
应用任务 (net_client)
  → esp_wifi_remote（同名注入 esp_wifi API）
  → esp-hosted（RPC）
  → SDIO 4-bit / 40 MHz
  → ESP32-C6 → AP → 服务器
```

## 目录

| 路径 | 内容 |
|---|---|
| `main/` | 实验源码；入口 `main.c` 的 `RUN_EXPERIMENT` |
| `main/wifi_sta.c` | Wi-Fi STA 初始化 + 事件组等 IP |
| `main/net_client.c` | TCP 客户端：非阻塞 connect、心跳、退避、故障注入 |
| `main/mjpeg_server.c` | 相机 → 硬件 JPEG → HTTP（`/jpg` 单帧、`/stream` MJPEG） |
| `main/camera_ui.c` | 相机直绘 + LVGL 局部叠加（30 fps） |
| `main/*_demo.c` | FreeRTOS 各主题实验 |
| `docs/` | 课程笔记与排障记录（图传见 `docs/13-相机网络图传.md`） |
| `sdkconfig.defaults` | P4+C6 SDIO / hosted、PSRAM、相机配置 |

## 快速开始

```bash
git clone <repo-url>
cd p4-lab
cp main/wifi_secrets.h.example main/wifi_secrets.h   # 填入 SSID/密码
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

`main/main.c` 里改 `RUN_EXPERIMENT` 切换实验，例如 `EXP_NET_CLIENT`（网络客户端）、`EXP_DUAL_CORE`（双核 IPC）、`EXP_CAM_UI`（相机 + LVGL 同屏）、`EXP_MJPEG`（图传）。

`EXP_MJPEG` 跑起来后，串口会打印 `板子地址：http://<IP>/`，浏览器打开：

| 路径 | 作用 |
|---|---|
| `/` | 测试页（内嵌连续流） |
| `/jpg` | 取一张新帧（JPEG 单图） |
| `/stream` | MJPEG 连续流（`multipart/x-mixed-replace`） |

> 首次构建需要联网，组件管理器会按 `main/idf_component.yml` 拉取 esp_hosted / esp_wifi_remote。

## 实测数据（2026-11-18 ~ 11-20）

| 项 | 结果 |
|---|---|
| DHCP → TCP 连接 | 0.96 s |
| 心跳周期 | 5.1~5.3 s（5 s + select 200 ms 粒度） |
| 断开检测 | 主动断开 20 ms；静默丢失 15.1 s |
| 重连退避 | 2.1 s（两次退避叠加，待优化到 ~1.1 s） |
| 重拨 → 拿到 IP | 3.7 s（关联 2.6 s + DHCP 1.1 s） |
| 拿到 IP → 连上服务器 | 0.25~0.37 s（DNS + TCP 握手） |
| 固件 | 1.05 MB / app 分区 4 MB（余 74%） |

## 实测数据（2026-11-21 ~ 11-23，显示 / 相机 / 图传）

| 项 | 结果 |
|---|---|
| 点屏 | JD9165 1024×600，DSI 2-lane 600 Mbps，DPI 51.2 MHz |
| 相机采集 | SC2336 1024×600 RGB565，30.0 fps |
| 相机 + LVGL 同屏 | 12 fps → **29~30 fps**（相机直绘 + LVGL PARTIAL 小缓冲） |
| 硬件 JPEG 编码 | 1024×600 RGB565 → 20~50 KB，**7~8 ms/帧** |
| `/jpg` 单帧 | 发送 44~397 ms（21~48 KB/张） |
| `/stream` MJPEG | **11~14 fps**，23~35 KB/帧，采集侧仍 30 fps |

## 已知限制

- 重连时退避等了两次（会话结束一次、等 IP 又一次），t2 约 2.1 s，可降到 ~1.1 s。
- 实验以手机热点为 AP，长时间运行会因热点省电/自动关闭而断，重连逻辑可自愈。
- 图传帧率受 Wi-Fi 带宽限制（不是编码慢）；降分辨率或用 PPA 缩放可再提帧率。
- 代码按实验组织，`RUN_EXPERIMENT` 是编译期选择器，不是运行时多模式。

## 许可

未声明许可证，作者保留权利。
