/*
 * queue_demo.c —— 队列实验（B3，对应视频 09-10）
 *
 * 场景：一个生产者任务每 10ms 往队列塞一个序号，一个消费者任务每 50ms 取一个。
 *      生产的 100/s，消费的 20/s → 队列必然被塞满 → 之后每次 send 都失败。
 *
 * 目的：让你亲眼看到三件事
 *   1. 队列满时 xQueueSend(..., 0) 会立刻返回失败（不等待、不丢队列里的东西）
 *   2. 丢包率可以算出来：稳态 ≈ (100-20)/100 = 80%
 *   3. 把消费者改成 5ms（200/s）后，丢包立刻变成 0 —— 说明队列长度治不了供需失衡
 *
 * 两个可切实验组：
 *   CONSUMER_PERIOD_MS = 50 → 慢消费，看丢包
 *   CONSUMER_PERIOD_MS =  5 → 快消费，看零丢包
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "queue_demo.h"

static const char *TAG = "QUEUE";

/*============================================================================
 * 实验参数
 *============================================================================*/
#define QUEUE_LENGTH            5       /* 队列能放几项 */
#define PRODUCER_PERIOD_MS      10      /* 生产者周期：100 个/秒 */
#define CONSUMER_PERIOD_MS      15     /* 消费者周期：20 个/秒（改成 5 就是第二组） */

#define PRODUCER_PRIO           4       /* 生产者的优先级——注意"生产者更快"要靠周期，不靠优先级 */
#define CONSUMER_PRIO           3
#define QUEUE_TASK_STACK        4096u
#define QUEUE_CORE              0
#define QUEUE_RUN_SECONDS       5
#define MAIN_PRIO_BOOST         5       /* 创建期间 main 要高于两个 worker */

/* 统计量：生产者/消费者各写各的，main 只在最后读，所以这里不额外加锁。
 * 真实工程里如果多方同时改统计量，就该用临界区或原子操作。 */
static volatile uint32_t s_sent_ok   = 0;   /* 成功入队 */
static volatile uint32_t s_sent_fail = 0;   /* 队列满，入队失败 */
static volatile uint32_t s_recv_cnt  = 0;   /* 成功出队 */
static volatile uint32_t s_max_depth = 0;   /* 队列曾达到的最大占用 */

static QueueHandle_t s_queue = NULL;

/*============================================================================
 * 生产者：每 10ms 生产一个递增序号
 * 关键点：xQueueSend 的超时参数是 0 —— 满了就不等，直接失败并计数
 *============================================================================*/
static void producer_task(void *arg)
{
    uint32_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t deadline_us = esp_timer_get_time() + (int64_t)QUEUE_RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        if (xQueueSend(s_queue, &seq, 0) == pdTRUE) {
            s_sent_ok++;
            seq++;

            /* 记录队列占用峰值：放在入队之后采样最准 */
            UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
            if (depth > s_max_depth) {
                s_max_depth = depth;
            }
        } else {
            /* 队列满：这次的数据直接丢掉（真实工程里也可以在这里改成丢最旧） */
            s_sent_fail++;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PRODUCER_PERIOD_MS));
    }

    ESP_LOGI(TAG, "生产者结束：成功 %u，失败 %u",
             (unsigned)s_sent_ok, (unsigned)s_sent_fail);

    vTaskDelete(NULL);
}

/*============================================================================
 * 消费者：每 CONSUMER_PERIOD_MS 取一个
 * 关键点：xQueueReceive 的超时也是 0 —— 空的就不等
 *============================================================================*/
static void consumer_task(void *arg)
{
    uint32_t value = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t deadline_us = esp_timer_get_time() + (int64_t)QUEUE_RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        if (xQueueReceive(s_queue, &value, 0) == pdTRUE) {
            s_recv_cnt++;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONSUMER_PERIOD_MS));
    }

    ESP_LOGI(TAG, "消费者结束：共取出 %u 个", (unsigned)s_recv_cnt);

    vTaskDelete(NULL);
}

/*============================================================================
 * 对外入口
 *============================================================================*/
void queue_demo_start(void)
{
    ESP_LOGI(TAG, "==== 队列实验 ====");
    ESP_LOGI(TAG, "队列长度 %d 项 × %u 字节；生产周期 %d ms，消费周期 %d ms，跑 %d 秒",
             QUEUE_LENGTH, (unsigned)sizeof(uint32_t),
             PRODUCER_PERIOD_MS, CONSUMER_PERIOD_MS, QUEUE_RUN_SECONDS);

    s_sent_ok = 0;
    s_sent_fail = 0;
    s_recv_cnt = 0;
    s_max_depth = 0;

    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(uint32_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败（内存不够？）");
        return;
    }

    /* 创建期间把 main 提到比两个 worker 都高，防止被抢占后后面的任务建不出来 */
    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(producer_task, "prod", QUEUE_TASK_STACK, NULL,
                                   PRODUCER_PRIO, NULL, QUEUE_CORE);
    ret |= xTaskCreatePinnedToCore(consumer_task, "cons", QUEUE_TASK_STACK, NULL,
                                   CONSUMER_PRIO, NULL, QUEUE_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    /* 等实验跑完（多留 0.5 秒余量，让两个任务把自己的收尾日志打完） */
    vTaskDelay(pdMS_TO_TICKS(QUEUE_RUN_SECONDS * 1000 + 500));

    uint32_t total_sent = s_sent_ok + s_sent_fail;
    uint32_t loss_pct   = (total_sent > 0) ? (s_sent_fail * 100u / total_sent) : 0;

    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "生产尝试 %u | 成功 %u | 失败(队列满) %u | 丢包率 %u%%",
             (unsigned)total_sent, (unsigned)s_sent_ok,
             (unsigned)s_sent_fail, (unsigned)loss_pct);
    ESP_LOGI(TAG, "消费 %u | 队列最大占用 %u / %d",
             (unsigned)s_recv_cnt, (unsigned)s_max_depth, QUEUE_LENGTH);

    vQueueDelete(s_queue);
    s_queue = NULL;

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
