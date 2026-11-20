/*
 * queue_advanced.c —— 队列进阶实验（B3 续）
 *
 * 一个文件四种模式，改顶部 QADV_MODE 切换：
 *   0 丢弃     生产者超时 0，满了丢新数据
 *   1 覆盖     队列长度 1 + xQueueOverwrite，永远保留最新值
 *   2 背压     生产者 portMAX_DELAY，被消费者拖慢，一个不丢
 *   3 队列集   两个生产者（10ms / 25ms）+ 一个消费者用 xQueueSelectFromSet 同时等两个队列
 *
 * 统一参数：生产周期 10ms（100/s），消费周期 20ms（50/s），跑 5 秒。
 * 预期（前三种模式）：
 *   丢弃：成功 ≈ 255，失败 ≈ 246，丢包率 ≈ 49%，队列会满（5/5）
 *   覆盖：成功 = 尝试 ≈ 500，失败 0，队列最大占用 1
 *   背压：失败 0，但生产者平均周期从 10ms 被拉到 ≈ 20ms（生产总量减半）
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "queue_advanced.h"

static const char *TAG = "QADV";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define QADV_MODE_DROP          0
#define QADV_MODE_OVERWRITE     1
#define QADV_MODE_BACKPRESSURE  2
#define QADV_MODE_QUEUE_SET     3


#define QADV_MODE             QADV_MODE_QUEUE_SET  
  /* ← 改这一行切换实验 */

/*============================================================================
 * ② 公共参数
 *============================================================================*/
#define PROD_PERIOD_MS          10      /* 生产 100/s */
#define CONS_PERIOD_MS          20      /* 消费 50/s */
#define QUEUE_LEN               5       /* 丢弃/背压模式用；覆盖模式强制为 1 */
#define RUN_SECONDS             5
#define TASK_STACK_BYTES        4096u
#define PROD_PRIO               4
#define CONS_PRIO               3
#define QADV_CORE               0
#define MAIN_PRIO_BOOST         5       /* 创建期间 main 必须高于两个 worker */
#define SET_MEMBER_LEN          3       /* 队列集模式下每个成员队列的长度 */
#define SET_PROD_A_PERIOD_MS    10      /* 生产者 A 的周期 */
#define SET_PROD_B_PERIOD_MS    25      /* 生产者 B 的周期 */

/*============================================================================
 * ③ 模块状态
 *============================================================================*/
#if QADV_MODE != QADV_MODE_QUEUE_SET
static QueueHandle_t s_queue = NULL;
#endif

static volatile uint32_t s_attempt    = 0;   /* 生产尝试次数 */
static volatile uint32_t s_ok         = 0;   /* 成功入队 */
static volatile uint32_t s_fail       = 0;   /* 入队失败 */
static volatile uint32_t s_consumed   = 0;   /* 成功出队 */
static volatile uint32_t s_max_depth  = 0;   /* 队列最大占用 */

static volatile int64_t  s_prod_elapsed_us = 0;  /* 生产者实际跑了多久 */
static volatile uint32_t s_prod_iters      = 0;  /* 生产者实际循环了多少次 */

#if QADV_MODE == QADV_MODE_QUEUE_SET
static QueueHandle_t     s_q_a = NULL;      /* 成员队列 A（快） */
static QueueHandle_t     s_q_b = NULL;      /* 成员队列 B（慢） */
static QueueSetHandle_t  s_set = NULL;      /* 队列集 */
static volatile uint32_t s_a_cnt = 0;       /* 从 A 取走多少个 */
static volatile uint32_t s_b_cnt = 0;       /* 从 B 取走多少个 */
#endif

/*============================================================================
 * ④ 公共小工具
 *============================================================================*/
/* 记一次入队后的队列占用峰值（队列集模式用不到） */
#if QADV_MODE != QADV_MODE_QUEUE_SET
static void track_depth(QueueHandle_t q)
{
    UBaseType_t depth = uxQueueMessagesWaiting(q);
    if (depth > s_max_depth) {
        s_max_depth = depth;
    }
}
#endif /* QADV_MODE != QADV_MODE_QUEUE_SET */

/*============================================================================
 * ⑤ 模式 0/1/2 的生产者与消费者
 *============================================================================*/
#if QADV_MODE != QADV_MODE_QUEUE_SET

static void producer_task(void *arg)
{
    uint32_t seq = 0;
    uint32_t iters = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t t_start = esp_timer_get_time();
    int64_t deadline_us = t_start + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        BaseType_t ret;

        s_attempt++;

#if QADV_MODE == QADV_MODE_OVERWRITE
        /* 覆盖：不进就不进，进去就覆盖旧的；队列长度必须是 1 */
        ret = xQueueOverwrite(s_queue, &seq);
#elif QADV_MODE == QADV_MODE_BACKPRESSURE
        /* 背压：满了就永远等，生产者会被消费者拖慢 */
        ret = xQueueSend(s_queue, &seq, portMAX_DELAY);
#else
        /* 丢弃：满了立刻失败，丢的是新数据 */
        ret = xQueueSend(s_queue, &seq, 0);
#endif

        if (ret == pdTRUE) {
            s_ok++;
            seq++;
            track_depth(s_queue);
        } else {
            s_fail++;
        }

        iters++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PROD_PERIOD_MS));
    }

    s_prod_elapsed_us = esp_timer_get_time() - t_start;
    s_prod_iters = iters;

    vTaskDelete(NULL);
}

static void consumer_task(void *arg)
{
    uint32_t value = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        if (xQueueReceive(s_queue, &value, 0) == pdTRUE) {
            s_consumed++;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONS_PERIOD_MS));
    }

    vTaskDelete(NULL);
}

#endif /* !QUEUE_SET 模式 */

/*============================================================================
 * ⑥ 模式 3：队列集——两个生产者 + 一个消费者
 *============================================================================*/
#if QADV_MODE == QADV_MODE_QUEUE_SET

/* 生产者 A：每 10ms 往 q_a 放一个（模拟串口这种高频小数据） */
static void set_producer_a(void *arg)
{
    uint32_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        (void)xQueueSend(s_q_a, &seq, 0);   /* 满了就算了，这里只看队列集能不能都收到 */
        seq++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SET_PROD_A_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

/* 生产者 B：每 25ms 往 q_b 放一个（模拟传感器这种低频数据） */
static void set_producer_b(void *arg)
{
    uint32_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        (void)xQueueSend(s_q_b, &seq, 0);
        seq++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SET_PROD_B_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

/* 消费者：一次只处理一个来源，靠 xQueueSelectFromSet 知道是谁有数据 */
static void set_consumer(void *arg)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        /* 最多等 50ms：没数据就转一圈看时间是否到点 */
        QueueHandle_t which = xQueueSelectFromSet(s_set, pdMS_TO_TICKS(50));
        if (which == NULL) {
            continue;   /* 超时，没任何队列有数据 */
        }

        uint32_t value = 0;
        if (which == s_q_a) {
            if (xQueueReceive(s_q_a, &value, 0) == pdTRUE) {
                s_a_cnt++;
            }
        } else if (which == s_q_b) {
            if (xQueueReceive(s_q_b, &value, 0) == pdTRUE) {
                s_b_cnt++;
            }
        }
    }
    vTaskDelete(NULL);
}

#endif /* QUEUE_SET 模式 */

/*============================================================================
 * ⑦ 对外入口
 *============================================================================*/
void queue_advanced_start(void)
{
#if QADV_MODE == QADV_MODE_QUEUE_SET
    ESP_LOGI(TAG, "==== 模式 3：队列集 ====");
    ESP_LOGI(TAG, "成员队列 A 周期 %d ms，B 周期 %d ms，每队列长 %d，集合容量 %d",
             SET_PROD_A_PERIOD_MS, SET_PROD_B_PERIOD_MS,
             SET_MEMBER_LEN, SET_MEMBER_LEN * 2);

    s_q_a = xQueueCreate(SET_MEMBER_LEN, sizeof(uint32_t));
    s_q_b = xQueueCreate(SET_MEMBER_LEN, sizeof(uint32_t));
    if (s_q_a == NULL || s_q_b == NULL) {
        ESP_LOGE(TAG, "创建成员队列失败");
        return;
    }

    /* 集合容量 = 所有成员"同时可能有数据"的上限，给保守值 = 长度之和 */
    s_set = xQueueCreateSet(SET_MEMBER_LEN * 2);
    if (s_set == NULL) {
        ESP_LOGE(TAG, "创建队列集失败");
        return;
    }
    xQueueAddToSet(s_q_a, s_set);
    xQueueAddToSet(s_q_b, s_set);

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(set_producer_a, "pa", TASK_STACK_BYTES, NULL,
                                   PROD_PRIO, NULL, QADV_CORE);
    ret |= xTaskCreatePinnedToCore(set_producer_b, "pb", TASK_STACK_BYTES, NULL,
                                   PROD_PRIO, NULL, QADV_CORE);
    ret |= xTaskCreatePinnedToCore(set_consumer, "sc", TASK_STACK_BYTES, NULL,
                                   CONS_PRIO, NULL, QADV_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(RUN_SECONDS * 1000 + 500));

    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "从 A 取走 %u 个，从 B 取走 %u 个",
             (unsigned)s_a_cnt, (unsigned)s_b_cnt);
    ESP_LOGI(TAG, "预期：A ≈ %d 个（5 秒 / %d ms），B ≈ %d 个（5 秒 / %d ms）",
             (RUN_SECONDS * 1000) / SET_PROD_A_PERIOD_MS, SET_PROD_A_PERIOD_MS,
             (RUN_SECONDS * 1000) / SET_PROD_B_PERIOD_MS, SET_PROD_B_PERIOD_MS);

    vQueueDelete(s_q_a);
    vQueueDelete(s_q_b);
    vQueueDelete(s_set);
    s_q_a = NULL;
    s_q_b = NULL;
    s_set = NULL;

#else /* 模式 0/1/2 */

    static const char *mode_name[] = { "丢弃", "覆盖", "背压" };
    int queue_len = (QADV_MODE == QADV_MODE_OVERWRITE) ? 1 : QUEUE_LEN;

    ESP_LOGI(TAG, "==== 模式 %d：%s ====", QADV_MODE, mode_name[QADV_MODE]);
    ESP_LOGI(TAG, "队列长度 %d，生产周期 %d ms（100/s），消费周期 %d ms（50/s），跑 %d 秒",
             queue_len, PROD_PERIOD_MS, CONS_PERIOD_MS, RUN_SECONDS);

    s_attempt = 0;
    s_ok = 0;
    s_fail = 0;
    s_consumed = 0;
    s_max_depth = 0;

    s_queue = xQueueCreate(queue_len, sizeof(uint32_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return;
    }

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(producer_task, "prod", TASK_STACK_BYTES, NULL,
                                   PROD_PRIO, NULL, QADV_CORE);
    ret |= xTaskCreatePinnedToCore(consumer_task, "cons", TASK_STACK_BYTES, NULL,
                                   CONS_PRIO, NULL, QADV_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(RUN_SECONDS * 1000 + 500));

    uint32_t loss_pct = (s_attempt > 0) ? (s_fail * 100u / s_attempt) : 0;
    int64_t avg_period_ms = (s_prod_iters > 0)
                            ? (s_prod_elapsed_us / (int64_t)s_prod_iters / 1000)
                            : 0;

    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "生产尝试 %u | 成功 %u | 失败 %u | 丢包率 %u%%",
             (unsigned)s_attempt, (unsigned)s_ok, (unsigned)s_fail, (unsigned)loss_pct);
    ESP_LOGI(TAG, "消费 %u | 队列最大占用 %u / %d",
             (unsigned)s_consumed, (unsigned)s_max_depth, queue_len);
    ESP_LOGI(TAG, "生产者平均周期 %lld ms（设定 %d ms）",
             (long long)avg_period_ms, PROD_PERIOD_MS);

    vQueueDelete(s_queue);
    s_queue = NULL;

#endif

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
