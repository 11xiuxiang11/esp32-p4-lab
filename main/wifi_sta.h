#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * wifi_sta —— P4 通过 C6 连 Wi-Fi（esp-hosted over SDIO）
 *
 * P4 自己没有 Wi-Fi 射频，靠 esp_wifi_remote 把 esp_wifi_* 调用转发给 C6。
 * 这里只做三件事：初始化、等 IP、必要时重新拨号。
 * 重连的节奏（退避）不在这里，在 net_client.c 的任务里。
 */

/* SSID/密码放在 main/wifi_secrets.h（已 gitignore），模板见 wifi_secrets.h.example */
#include "wifi_secrets.h"

/* 初始化 STA 并启动 Wi-Fi。
 * 返回 true  = 初始化完成（不代表已经连上 AP）
 * 返回 false = 初始化失败（NVS / netif / esp_wifi 某一步失败） */
bool wifi_sta_start(void);

/* 等拿到 IP，最多等 timeout_ms。
 * 返回 true = 有 IP；false = 超时（还没连上，或者 DHCP 没通） */
bool wifi_sta_wait_ip(uint32_t timeout_ms);

/* 让驱动重新发起一次连接（非阻塞，只发命令）。
 * 什么时候用：等 IP 超时后，由上层决定等多久再拨。 */
void wifi_sta_kick(void);

/* 把当前 IPv4 地址写成字符串（"192.168.1.23"），方便打印访问地址。
 * 返回 true = 有地址且写进了 out；false = 还没拿到 IP（或 netif 没建）。 */
bool wifi_sta_get_ip_str(char *out, size_t out_len);
