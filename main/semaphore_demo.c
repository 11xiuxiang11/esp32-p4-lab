/*
 * semaphore_demo.c —— 信号量实验（B4，对应视频 11-12）
 *
 * 两种模式，改顶部 SEM_MODE 切换：
 *
 *   0 = 二值信号量做「事件通知」
 *        worker 任务阻塞在 xSemaphoreTake(bin, portMAX_DELAY)
 *        producer 任务每 500ms 调一次 xSemaphoreGive(bin)（模拟中断上报事件）
 *        worker 被唤醒后，测「从 give 到真正醒来」的延迟
 *        预期：worker 优先级更高 → 延迟通常 < 1ms
 *
 *   1 = 计数信号量做「资源池」
 *        创建 xSemaphoreCreateCounting(3, 3)：3 个资源槽
 *        5 个 worker 竞争这 3 个槽，每个占用 200ms
 *        预期：最大并发 = 3（第 4、5 个任务必须等别人归还）
 *
 * 注意：二值信号量【创建后是空的】，第一次 take 会阻塞 —— 这和互斥量相反。
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "semaphore_demo.h"

static const char *TAG = "SEM";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define SEM_MODE_EVENT      0
#define SEM_MODE_POOL       1

#define SEM_MODE            SEM_MODE_EVENT     /* ← 改这一行切换实验 */

/*============================================================================
 * ② 公共参数
 *============================================================================*/
#define TASK_STACK_BYTES        4096u
#define SEM_CORE                0
#define MAIN_PRIO_BOOST         7

/*============================================================================
 * ③ 模式 0：二值信号量做事件通知
 *============================================================================*/
#if SEM_MODE == SEM_MODE_EVENT

#define EVENT_COUNT             10      /* 一共给 10 次事件 */
#define EVENT_PERIOD_MS         500     /* 每 500ms 一次 */
#define WORKER_PRIO             5       /* worker 必须【高于】producer，才能立刻被唤醒 */
#define PRODUCER_PRIO           3
#define TAKE_TIMEOUT_MS         2000

static SemaphoreHandle_t s_bin = NULL;
static volatile int64_t  s_give_us = 0;   /* producer 最近一次 give 的时刻 */

/* 模拟"事件源"：可以是中断、也可以像这里一样是另一个任务 */
static void event_producer(void *arg)
{
    for (int i = 0; i < EVENT_COUNT; i++) {
        vTaskDelay(pdMS_TO_TICKS(EVENT_PERIOD_MS));

        /* 先记时间，再 give —— 顺序反了测出来的延迟就没意义 */
        s_give_us = esp_timer_get_time();
        xSemaphoreGive(s_bin);
    }

    ESP_LOGI(TAG, "事件源结束，共发出 %d 次", EVENT_COUNT);
    vTaskDelete(NULL);
}

/* 消费者：平时阻塞在 take 上，被 give 唤醒 */
static void event_worker(void *arg)
{
    int got = 0;

    while (got < EVENT_COUNT) {
        /* portMAX_DELAY：没事件就一直睡，完全不占 CPU */
        if (xSemaphoreTake(s_bin, pdMS_TO_TICKS(TAKE_TIMEOUT_MS)) == pdTRUE) {
            int64_t latency_us = esp_timer_get_time() - s_give_us;
            got++;
            ESP_LOGI(TAG, "第 %d 次事件，唤醒延迟 %lld us", got,
                     (long long)latency_us);
        } else {
            ESP_LOGW(TAG, "等待超时（%d ms），退出", TAKE_TIMEOUT_MS);
            break;
        }
    }

    ESP_LOGI(TAG, "worker 退出，共处理 %d 次", got);
    vTaskDelete(NULL);
}

#endif /* SEM_MODE_EVENT */

/*============================================================================
 * ④ 模式 1：计数信号量做资源池
 *============================================================================*/
#if SEM_MODE == SEM_MODE_POOL

#define RES_TOTAL               3       /* 资源槽总数 */
#define RES_WORKERS             5       /* 竞争资源的任务数 */
#define RES_HOLD_MS             200     /* 每个任务占用多久 */
#define RES_WORKER_PRIO         4
#define RES_STACK_BYTES         4096u

static SemaphoreHandle_t s_pool = NULL;

/* 统计：同时占用资源的最大数量（用临界区保护，复用 B2-1 的知识） */
static portMUX_TYPE      s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int      s_in_use = 0;
static volatile int      s_max_in_use = 0;
static volatile int      s_done = 0;

static void pool_worker(void *arg)
{
    const char *name = (const char *)arg;

    /* 拿资源：拿不到就等（这里的等待是"排队"，不是"失败"） */
    if (xSemaphoreTake(s_pool, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "%s 拿资源失败", name);
        vTaskDelete(NULL);
        return;
    }

    /* 记录当前占用数 */
    portENTER_CRITICAL(&s_stat_mux);
    s_in_use++;
    int cur = s_in_use;
    if (cur > s_max_in_use) {
        s_max_in_use = cur;
    }
    portEXIT_CRITICAL(&s_stat_mux);

    ESP_LOGI(TAG, "%s 拿到资源（当前占用 %d 个）", name, cur);

    vTaskDelay(pdMS_TO_TICKS(RES_HOLD_MS));   /* 模拟使用资源 */

    portENTER_CRITICAL(&s_stat_mux);
    s_in_use--;
    s_done++;
    portEXIT_CRITICAL(&s_stat_mux);

    xSemaphoreGive(s_pool);                   /* 归还资源 */
    ESP_LOGI(TAG, "%s 归还资源", name);

    vTaskDelete(NULL);
}

#endif /* SEM_MODE_POOL */

/*============================================================================
 * ⑤ 对外入口
 *============================================================================*/
void semaphore_demo_start(void)
{
#if SEM_MODE == SEM_MODE_EVENT
    ESP_LOGI(TAG, "==== 模式 0：二值信号量做事件通知 ====");

    s_bin = xSemaphoreCreateBinary();
    if (s_bin == NULL) {
        ESP_LOGE(TAG, "创建二值信号量失败");
        return;
    }

    ESP_LOGI(TAG, "刚创建时计数 = %u（0 表示空，take 会阻塞）",
             (unsigned)uxSemaphoreGetCount(s_bin));

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(event_worker, "evt_worker", TASK_STACK_BYTES, NULL,
                                   WORKER_PRIO, NULL, SEM_CORE);
    ret |= xTaskCreatePinnedToCore(event_producer, "evt_source", TASK_STACK_BYTES, NULL,
                                   PRODUCER_PRIO, NULL, SEM_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    /* 等事件跑完：10 次 × 500ms = 5 秒，多留 1 秒 */
    vTaskDelay(pdMS_TO_TICKS(EVENT_COUNT * EVENT_PERIOD_MS + 1000));

    vSemaphoreDelete(s_bin);
    s_bin = NULL;

#elif SEM_MODE == SEM_MODE_POOL
    ESP_LOGI(TAG, "==== 模式 1：计数信号量做资源池 ====");
    ESP_LOGI(TAG, "%d 个任务竞争 %d 个资源槽，每个占用 %d ms",
             RES_WORKERS, RES_TOTAL, RES_HOLD_MS);

    s_pool = xSemaphoreCreateCounting(RES_TOTAL, RES_TOTAL);
    if (s_pool == NULL) {
        ESP_LOGE(TAG, "创建计数信号量失败");
        return;
    }

    ESP_LOGI(TAG, "刚创建时计数 = %u（= 可用资源数）",
             (unsigned)uxSemaphoreGetCount(s_pool));

    static const char *names[RES_WORKERS] = { "W0", "W1", "W2", "W3", "W4" };

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    for (int i = 0; i < RES_WORKERS; i++) {
        ret |= xTaskCreatePinnedToCore(pool_worker, names[i], RES_STACK_BYTES,
                                       (void *)names[i], RES_WORKER_PRIO, NULL,
                                       SEM_CORE);
    }

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    /* 5 个任务、3 个槽、每个 200ms → 最多两轮 400ms，留 2 秒足够 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "完成任务 %d / %d，同时占用资源的最大值 = %d（应为 %d），剩余计数 = %u",
             s_done, RES_WORKERS, s_max_in_use, RES_TOTAL,
             (unsigned)uxSemaphoreGetCount(s_pool));

    vSemaphoreDelete(s_pool);
    s_pool = NULL;
#else
#error "SEM_MODE 只能是 SEM_MODE_EVENT 或 SEM_MODE_POOL"
#endif

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
