/*
 * net_client.c —— lwIP socket 客户端实验（P6）
 *
 * 编译验证：idf.bat build
 * 配置    ：RUN_EXPERIMENT = EXP_NET_CLIENT (12)
 * 结果    ：0 error / 0 warning
 * 日期    ：2026-11-18
 *
 * 数据通路：
 *   net_client（本文件）→ wifi_sta → esp_wifi_remote → esp-hosted
 *   → SDIO → C6 → AP → 服务器
 *
 * 流程：
 *   等 Wi-Fi 拿到 IP → getaddrinfo → 非阻塞 connect → 心跳 + recv
 *   → 断线 → 指数退避 → 回到「等 IP / 重拨」
 *
 * 改 SSID/密码：wifi_sta.h 顶部两行。
 * 改服务器：net_client.h 的 NET_CLIENT_DEFAULT_HOST/PORT。
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "net_client.h"
#include "wifi_sta.h"

static const char *TAG = "NET";

/*============================================================================
 * 实验参数 —— 改这里，不要改函数里的裸数字
 *============================================================================*/
#define NET_CONNECT_TIMEOUT_MS    5000   /* 非阻塞 connect 自己限时 */
#define NET_WIFI_IP_WAIT_MS      10000   /* 等 Wi-Fi 拿 IP 的上限 */
#define NET_HEARTBEAT_PERIOD_MS   5000   /* 每 5 秒发一次 PING */
#define NET_HEARTBEAT_TIMEOUT_MS 15000   /* 15 秒收不到任何数据就判死 */
#define NET_BACKOFF_MIN_MS        1000
#define NET_BACKOFF_MAX_MS       60000
#define NET_BACKOFF_JITTER_PCT      20   /* 额外随机 0~20%，防同步重连 */
#define NET_RECV_BUF_SIZE          256
#define NET_SELECT_STEP_MS         200   /* select 每次等 200ms，便于查心跳 */
#define NET_TASK_STACK            4096
#define NET_TASK_PRIO                5

/* 故障注入：启动 30 秒后开始，每 30 秒主动断开一次 Wi-Fi，共 3 次。
 * 用来观察重连状态机。不想注入就设成 0。 */
#define FAULT_COUNT               3
#define FAULT_FIRST_DELAY_MS  30000
#define FAULT_INTERVAL_MS     30000

static const char NET_HEARTBEAT_MSG[] = "PING\n";

typedef struct {
    const char *host;
    uint16_t    port;
} net_cfg_t;

static net_cfg_t s_cfg = { NET_CLIENT_DEFAULT_HOST, NET_CLIENT_DEFAULT_PORT };

/*============================================================================
 * 退避：翻倍 + 上限 + 抖动
 *============================================================================*/

/* 给等待时间加 0~20% 的随机量。
 * 为什么：一百台设备同时断线，如果都按 1/2/4 秒同步重连，会把 AP 打爆。 */
static uint32_t backoff_with_jitter(uint32_t base_ms)
{
    uint32_t extra = esp_random() % (base_ms * NET_BACKOFF_JITTER_PCT / 100 + 1);
    return base_ms + extra;
}

/* 下一次退避时间：翻倍，封顶。 */
static uint32_t next_backoff(uint32_t current_ms)
{
    uint32_t next = current_ms * 2;
    return (next > NET_BACKOFF_MAX_MS) ? NET_BACKOFF_MAX_MS : next;
}

/*============================================================================
 * select 小工具：等可读 / 等可写
 *============================================================================*/

/* 等 fd 可读或可写，最多 timeout_ms。
 * 返回 true = 就绪；false = 超时或出错。 */
static bool wait_event(int sock, bool want_write, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int ret = want_write ? select(sock + 1, NULL, &set, NULL, &tv)
                         : select(sock + 1, &set, NULL, NULL, &tv);
    return ret > 0;
}

/*============================================================================
 * 发送：非阻塞 send 可能只收一部分，必须循环
 *============================================================================*/

/* 把 len 字节全部发出去。
 * 返回 true = 全部进了发送缓冲；false = 出错或缓冲一直满。 */
static bool send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            if (!wait_event(sock, true, NET_CONNECT_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "发送缓冲一直满");
                return false;
            }
            continue;
        }
        ESP_LOGW(TAG, "send 失败: errno=%d", errno);
        return false;
    }
    return true;
}

/*============================================================================
 * 连接：非阻塞 connect + 自己限时
 *============================================================================*/

/* 非阻塞 connect，最多等 NET_CONNECT_TIMEOUT_MS。
 * 成功返回 fd（>=0）；失败返回 -1，原因见日志里的 errno/so_error。 */
static int connect_with_timeout(const struct sockaddr *addr, socklen_t addr_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket 失败: errno=%d", errno);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "设置非阻塞失败: errno=%d", errno);
        close(sock);
        return -1;
    }

    int ret = connect(sock, addr, addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect 立刻失败: errno=%d", errno);
        close(sock);
        return -1;
    }

    if (ret < 0) {
        /* EINPROGRESS：正在握手，等可写再问结果 */
        if (!wait_event(sock, true, NET_CONNECT_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "connect 超时");
            close(sock);
            return -1;
        }

        int so_error = 0;
        socklen_t opt_len = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) < 0 ||
            so_error != 0) {
            ESP_LOGW(TAG, "connect 失败: so_error=%d", so_error);
            close(sock);
            return -1;
        }
    }
    return sock;
}

/*============================================================================
 * 一次完整通话：解析 → 连接 → 收发 → 关闭
 *============================================================================*/

/* 跑一次会话。
 * 返回 true = 业务成功过（收到过至少一次数据），用来重置退避。
 * 返回 false = 没收到任何数据就结束（连不上、被拒、心跳超时）。 */
static bool tcp_session_once(void)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", (int)s_cfg.port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;

    int gai = getaddrinfo(s_cfg.host, port_str, &hints, &res);
    if (gai != 0) {
        ESP_LOGE(TAG, "getaddrinfo 失败: %d（DNS 没通？）", gai);
        return false;
    }

    int sock = connect_with_timeout(res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (sock < 0) {
        return false;
    }
    ESP_LOGI(TAG, "连上了 %s:%u", s_cfg.host, (unsigned)s_cfg.port);

    bool received_any = false;
    int64_t last_rx_us = esp_timer_get_time();
    int64_t last_tx_us = last_rx_us;
    uint8_t buf[NET_RECV_BUF_SIZE];

    while (true) {
        if (wait_event(sock, false, NET_SELECT_STEP_MS)) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n == 0) {
                /* 对端正常关闭：FIN。不是错误，这通电话说完了 */
                ESP_LOGI(TAG, "对端关闭（FIN）");
                break;
            }
            if (n < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    continue;   /* select 和 recv 之间的正常竞争 */
                }
                ESP_LOGW(TAG, "recv 失败: errno=%d", errno);
                break;
            }
            received_any = true;
            last_rx_us = esp_timer_get_time();
            ESP_LOGI(TAG, "收到 %d 字节", (int)n);
        }

        int64_t now_us = esp_timer_get_time();

        if (now_us - last_tx_us >= (int64_t)NET_HEARTBEAT_PERIOD_MS * 1000) {
            if (!send_all(sock, NET_HEARTBEAT_MSG, sizeof(NET_HEARTBEAT_MSG) - 1)) {
                break;
            }
            last_tx_us = now_us;
        }

        if (now_us - last_rx_us >= (int64_t)NET_HEARTBEAT_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "心跳超时，判定连接已死");
            break;
        }
    }

    close(sock);
    return received_any;
}

/*============================================================================
 * 故障注入：定时主动断开 Wi-Fi
 *============================================================================*/

/* 为什么用独立任务：要 vTaskDelay，不能放进事件回调（会堵住事件系统） */
static void fault_inject_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(FAULT_FIRST_DELAY_MS));

    for (int i = 0; i < FAULT_COUNT; i++) {
        ESP_LOGW(TAG, "故障注入：断开 Wi-Fi（%d/%d）", i + 1, FAULT_COUNT);
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_disconnect: 0x%x", err);
        }
        vTaskDelay(pdMS_TO_TICKS(FAULT_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "故障注入结束");
    vTaskDelete(NULL);
}

/*============================================================================
 * 主循环
 *============================================================================*/

/* 后台任务：等 IP → 一次会话 → 退避 → 再连。
 * 退避只在「业务成功」时归零；connect 成功不算。 */
static void net_client_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = NET_BACKOFF_MIN_MS;

    ESP_LOGI(TAG, "目标 %s:%u，先等 Wi-Fi 拿到 IP",
             s_cfg.host, (unsigned)s_cfg.port);

    while (true) {
        if (!wifi_sta_wait_ip(NET_WIFI_IP_WAIT_MS)) {
            /* Wi-Fi 层没通：按退避节奏重拨，不在事件回调里等 */
            uint32_t wait_ms = backoff_with_jitter(backoff_ms);
            ESP_LOGW(TAG, "Wi-Fi 没拿到 IP，%u ms 后重拨", (unsigned)wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            backoff_ms = next_backoff(backoff_ms);
            wifi_sta_kick();
            ESP_LOGI(TAG, "已发起重拨");
            continue;
        }

        bool healthy = tcp_session_once();

        uint32_t wait_ms = healthy ? NET_BACKOFF_MIN_MS : backoff_ms;
        backoff_ms = healthy ? NET_BACKOFF_MIN_MS : next_backoff(backoff_ms);

        wait_ms = backoff_with_jitter(wait_ms);
        ESP_LOGI(TAG, "%u ms 后重试", (unsigned)wait_ms);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

/*============================================================================
 * 对外入口
 *============================================================================*/

void net_client_start(const char *host, uint16_t port)
{
    if (host != NULL) {
        s_cfg.host = host;
    }
    if (port != 0) {
        s_cfg.port = port;
    }

    /* Wi-Fi 初始化失败就不要往下走：没有 netif，socket 也连不出去 */
    if (!wifi_sta_start()) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败，实验不启动");
        return;
    }

    BaseType_t ok = xTaskCreate(net_client_task, "net_client",
                                NET_TASK_STACK, NULL, NET_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 net_client 任务失败");
    }

    if (FAULT_COUNT > 0) {
        xTaskCreate(fault_inject_task, "net_fault", 3072, NULL,
                    NET_TASK_PRIO - 1, NULL);
    }
}
