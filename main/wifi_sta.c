/*
 * wifi_sta.c —— P4 通过 C6 连 Wi-Fi（esp-hosted over SDIO）
 *
 * 编译验证：同 net_client.c 的构建（idf.bat build，0 warning）
 * 日期    ：2026-11-18
 *
 * 数据通路：
 *   应用 --esp_wifi_*--> esp_wifi_remote（替换 esp_wifi 的头文件）
 *        --> esp-hosted（RPC）--> SDIO --> C6 --> 空口
 *
 * 事件回调只做“记录”，不做重连决策：
 *   重拨的节奏（退避、等多久）在 net_client.c 的任务里。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi_sta.h"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_events = NULL;
#define WIFI_GOT_IP_BIT       BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT1
/* Wi-Fi / IP 事件：只记录状态，不阻塞、不循环 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 第一次拨号：发命令就返回，不等结果 */
        wifi_sta_kick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt =
            (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "断开, reason=%d", evt->reason);
        xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        /* 不在这里 vTaskDelay / 重拨：那是事件任务，一堵全堵 */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_GOT_IP_BIT);
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "IP 丢失");
        xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT);
    }
}

bool wifi_sta_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase: 0x%x", ret);
            return false;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: 0x%x", ret);
        return false;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return false;
    }

    /* lwIP 的 tcpip_thread 由 esp_netif_init 拉起 */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: 0x%x", ret);
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环: 0x%x", ret);
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "创建默认 STA netif 失败");
        return false;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: 0x%x", ret);
        return false;
    }

    esp_event_handler_instance_t inst_wifi;
    esp_event_handler_instance_t inst_got_ip;
    esp_event_handler_instance_t inst_lost_ip;

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL,
                                              &inst_wifi);
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler, NULL,
                                                  &inst_got_ip);
    }
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                  &wifi_event_handler, NULL,
                                                  &inst_lost_ip);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册事件回调失败: 0x%x", ret);
        return false;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_STA_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_STA_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: 0x%x", ret);
        return false;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: 0x%x", ret);
        return false;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: 0x%x", ret);
        return false;
    }

    ESP_LOGI(TAG, "STA 已启动，目标 SSID=\"%s\"", WIFI_STA_SSID);
    return true;
}

bool wifi_sta_wait_ip(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }

    /* 两盏灯任意一盏亮就醒：拿到 IP，或者被断开叫醒 */
    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_GOT_IP_BIT) {
        return true;
    }

    /* 被“断开”叫醒：熄掉这盏灯，否则下一次一进来又立刻返回 */
    if (bits & WIFI_DISCONNECTED_BIT) {
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    }
    return false;
}

void wifi_sta_kick(void)
{
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: 0x%x", ret);
    }
}
