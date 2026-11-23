#pragma once

/*
 * mjpeg_server —— P5-3：Wi-Fi 图传（第一步：HTTP 单帧 /jpg）
 *
 * 相机 + 硬件 JPEG 编码 + esp_http_server，浏览器打开 http://<板子IP>/jpg
 * 就得到一张 1024x600 的 JPEG。下一步 /stream 复用同一套取帧/编码/投递。
 */

/* 建任务跑完整启动流程：连 Wi-Fi → 等 IP → 初始化相机/JPEG → 起 HTTP 服务。
 * 返回后不阻塞调用者；失败只在日志里报，不影响其它实验。 */
void mjpeg_server_start(void);
