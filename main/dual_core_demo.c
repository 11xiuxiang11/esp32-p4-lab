/*
 * dual_core_demo.c —— P4 双核：任务绑核 + 核间通信开销测量（双芯第二课）
 *
 * 实验原理：
 *   任务 A（绑 core X）把 1 个 uint32 令牌发到队列 q_a2b；
 *   任务 B（绑 core Y）收到后立刻把令牌发回队列 q_b2a；
 *   A 收到后记为一次往返，计时用 esp_timer_get_time()（微秒级）。
 *
 * 两种模式：
 *   同核：A、B 都在 core0 —— 通信靠同一个核的调度，开销小
 *   跨核：A 在 core0、B 在 core1 —— 需要跨核唤醒，开销大
 *
 * 注意：
 *   1) 测的是"队列往返延迟"，不是带宽；
 *   2) 测量期间关掉日志，避免串口干扰；
 *   3) 结果看平均值即可，单次抖动大。
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dual_core_demo.h"

static const char *TAG = "DCORE";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define DC_MODE_SAME_CORE      0
#define DC_MODE_CROSS_CORE     1

#define DC_MODE                DC_MODE_CROSS_CORE   /* ← 改这一行 */

/*============================================================================
 * ② 参数
 *============================================================================*/
#define DC_ROUNDS              20000u    /* 往返次数 */
#define DC_TASK_PRIO           6
#define DC_STACK_BYTES         4096u
#define DC_CORE_A              0
#if DC_MODE == DC_MODE_SAME_CORE
#define DC_CORE_B              0
#else
#define DC_CORE_B              1
#endif

static QueueHandle_t s_q_a2b = NULL;
static QueueHandle_t s_q_b2a = NULL;

/*============================================================================
 * ③ 任务 B：收到就回发
 *============================================================================*/
static void dc_task_b(void *arg)
{
    uint32_t token = 0;

    while (1) {
        if (xQueueReceive(s_q_a2b, &token, portMAX_DELAY) == pdTRUE) {
            /* 原样回发；发送阻塞时间给 0，队列空时不会卡住 */
            xQueueSend(s_q_b2a, &token, 0);
        }
    }
}

/*============================================================================
 * ④ 任务 A：发令牌、计时、收令牌
 *============================================================================*/
static void dc_task_a(void *arg)
{
    const uint32_t rounds = DC_ROUNDS;
    uint32_t token = 0;
    uint32_t got = 0;
    uint32_t miss = 0;
    int64_t t0 = 0, t1 = 0;

    vTaskDelay(pdMS_TO_TICKS(100));      /* 等 B 起来 */
    ESP_LOGI(TAG, "开始 %u 次往返…", (unsigned)rounds);

    t0 = esp_timer_get_time();           /* 微秒 */

    for (uint32_t i = 0; i < rounds; i++) {
        token = i;
        xQueueSend(s_q_a2b, &token, portMAX_DELAY);

        if (xQueueReceive(s_q_b2a, &got, portMAX_DELAY) == pdTRUE) {
            if (got != i) {
                miss++;
            }
        } else {
            miss++;
        }
    }

    t1 = esp_timer_get_time();

    /* 总耗时（微秒）换算成"每次往返多少纳秒" */
    int64_t total_us = t1 - t0;
    int64_t ns_per_round = (total_us * 1000) / (int64_t)rounds;

    ESP_LOGI(TAG, "模式=%s  往返 %u 次  总耗时 %lld us  单次 %lld ns  异常 %u",
             (DC_MODE == DC_MODE_SAME_CORE) ? "同核" : "跨核",
             (unsigned)rounds, (long long)total_us,
             (long long)ns_per_round, (unsigned)miss);

    vTaskDelete(NULL);
}

/*============================================================================
 * ⑤ 入口
 *============================================================================*/
void dual_core_demo_start(void)
{
    ESP_LOGI(TAG, "==== 双核实验：模式=%s ====",
             (DC_MODE == DC_MODE_SAME_CORE) ? "同核（都 core0）" : "跨核（core0 + core1）");
    ESP_LOGI(TAG, "A 绑 core%d，B 绑 core%d，往返 %u 次",
             DC_CORE_A, DC_CORE_B, (unsigned)DC_ROUNDS);

    s_q_a2b = xQueueCreate(1, sizeof(uint32_t));
    s_q_b2a = xQueueCreate(1, sizeof(uint32_t));
    if (s_q_a2b == NULL || s_q_b2a == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return;
    }

    /* 先把 B 建出来，避免 A 先跑导致第一次等待超时 */
    xTaskCreatePinnedToCore(dc_task_b, "dc_b", DC_STACK_BYTES, NULL,
                            DC_TASK_PRIO + 1, NULL, DC_CORE_B);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskCreatePinnedToCore(dc_task_a, "dc_a", DC_STACK_BYTES, NULL,
                            DC_TASK_PRIO, NULL, DC_CORE_A);

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "==== 实验结束 ====");
}
