#pragma once

#include <stdint.h>

/*
 * net_client —— lwIP socket 客户端实验（P6）
 *
 * 链路：Wi-Fi(hosted) → 等 IP → getaddrinfo → 非阻塞 connect
 *       → 心跳 + recv → 断线 → 指数退避
 * 现在 host 用域名，顺便验证 DNS。
 */

/* 实验默认参数：改这里，不要在 .c 里写裸数字 */
#define NET_CLIENT_DEFAULT_HOST "tcpbin.com"
#define NET_CLIENT_DEFAULT_PORT 4242

/* 启动 TCP 客户端实验。
 * host : 服务器地址（域名或点分 IP）
 * port : 服务器端口
 * 返回 : 无
 * 失败 : esp_netif_init 失败或任务创建失败时只打日志
 */
void net_client_start(const char *host, uint16_t port);
