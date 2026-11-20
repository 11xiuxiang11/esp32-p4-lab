/*
 * intr_demo.c —— 中断级调用实验（B6，对应视频 17）
 *
 * 用 GPTimer 的 alarm 中断（1ms 一次），演示"中断只通知、任务干活"的标准模式：
 *
 *   硬件定时器每 1ms 触发中断
 *        ↓
 *   ISR（IRAM_ATTR，几微秒）：记录时间戳 + vTaskNotifyGiveFromISR + 返回 woken
 *        ↓  （返回 true 时驱动自己调 portYIELD_FROM_ISR）
 *   任务：ulTaskNotifyTake 醒来，算出"从 ISR 执行到任务被唤醒"的延迟
 *
 * 预期：
 *   - ISR 每秒约 1000 次（1ms 周期）
 *   - 任务唤醒延迟：几十微秒量级（同核、任务优先级高）
 *   - 若把 WORKER_PRIO 降到比别的任务低，或加干扰，延迟会变大
 *
 * 注意几个硬性约束：
 *   1. gptimer 的 on_alarm 回调必须加 IRAM_ATTR（驱动会校验 esp_ptr_in_iram）
 *   2. 回调返回 true = "我唤醒了更高优先级任务，请在退出时切换"
 *   3. ISR 里不能 ESP_LOGI、不能 vTaskDelay、不能 malloc
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "intr_demo.h"

static const char *TAG = "INTR";

/*============================================================================
 * ① 参数
 *============================================================================*/
#define ISR_PERIOD_US       1000        /* 1ms 一次中断 */
#define RUN_SECONDS         3
#define WORKER_PRIO         6
#define WORKER_STACK        4096u
#define INTR_CORE           0
#define MAIN_PRIO_BOOST     7

/*============================================================================
 * ② 模块状态
 *============================================================================*/
static TaskHandle_t      s_worker = NULL;
static gptimer_handle_t  s_timer  = NULL;

static volatile uint32_t s_isr_cnt      = 0;   /* ISR 执行了多少次 */
static volatile uint32_t s_wake_cnt     = 0;   /* 任务被唤醒多少次 */
static volatile uint32_t s_timeout_cnt  = 0;   /* 任务等待超时次数 */
static volatile int64_t  s_last_isr_us  = 0;   /* ISR 里记录的时间戳 */

static volatile int64_t  s_lat_sum_us   = 0;
static volatile int64_t  s_lat_min_us   = 1000000;
static volatile int64_t  s_lat_max_us   = 0;

/*============================================================================
 * ③ ISR：只做"记录 + 通知"
 *
 * IRAM_ATTR 必须加：gptimer 驱动要求回调在 IRAM 内
 * （Flash 里的代码在 cache 被关闭时取不到，中断里就会崩）
 *============================================================================*/
static bool IRAM_ATTR on_alarm(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t *edata,
                               void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    BaseType_t higher_prio_woken = pdFALSE;

    s_isr_cnt++;

    /* esp_timer_get_time() 是 ISR 安全的（基于 systimer，不涉及阻塞） */
    s_last_isr_us = esp_timer_get_time();

    /* 先取局部句柄并判空：任务可能已经退出（句柄被置 NULL），
     * 否则就是往已删除的任务发通知 —— 野指针 */
    TaskHandle_t target = s_worker;
    if (target != NULL) {
        /* 通知任务：若被唤醒的任务优先级更高，内核把 woken 置 pdTRUE */
        vTaskNotifyGiveFromISR(target, &higher_prio_woken);
    }

    /* 返回 true → 驱动在 ISR 末尾调用 portYIELD_FROM_ISR()，立刻切换 */
    return (higher_prio_woken == pdTRUE);
}

/*============================================================================
 * ④ 任务：真正干活的地方
 *============================================================================*/
static void intr_worker(void *arg)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;

    while (esp_timer_get_time() < deadline_us) {
        /* 等通知；超时 200ms 只是为了能定期检查 deadline，不是业务超时 */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (notified == 0) {
            s_timeout_cnt++;
            continue;
        }

        /* 从 ISR 记录时间戳，到任务真正跑到这里，隔了多久 */
        int64_t latency_us = esp_timer_get_time() - s_last_isr_us;
        s_wake_cnt++;
        s_lat_sum_us += latency_us;
        if (latency_us < s_lat_min_us) {
            s_lat_min_us = latency_us;
        }
        if (latency_us > s_lat_max_us) {
            s_lat_max_us = latency_us;
        }

        /* 每 500 次打印一次，避免日志本身影响测量 */
        if ((s_wake_cnt % 500) == 0) {
            ESP_LOGI(TAG, "已处理 %u 次，本次唤醒延迟 %lld us",
                     (unsigned)s_wake_cnt, (long long)latency_us);
        }
    }

    s_worker = NULL;      /* 先摘掉句柄，之后 ISR 就不会再通知本任务 */
    vTaskDelete(NULL);
}

/*============================================================================
 * ⑤ 对外入口
 *============================================================================*/
void intr_demo_start(void)
{
    ESP_LOGI(TAG, "==== 中断级调用实验 ====");
    ESP_LOGI(TAG, "硬件定时器每 %d us 触发一次中断，跑 %d 秒", ISR_PERIOD_US, RUN_SECONDS);

    /* ---- 1. 配置 GPTimer：1MHz 计数（1 个计数 = 1us），1ms 报警，自动重装 ---- */
    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    esp_err_t err = gptimer_new_timer(&timer_cfg, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer 失败: 0x%x", err);
        return;
    }

    /* ---- 2. 注册中断回调 ---- */
    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_alarm,
    };
    err = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册回调失败: 0x%x（回调是否忘了 IRAM_ATTR？）", err);
        return;
    }

    /* ---- 3. 设置报警：每 1ms 触发一次并自动重装 ---- */
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = ISR_PERIOD_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    err = gptimer_set_alarm_action(s_timer, &alarm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置报警失败: 0x%x", err);
        return;
    }

    /* ---- 4. 建任务（先建任务，再启动定时器，避免 ISR 通知到空句柄） ---- */
    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = xTaskCreatePinnedToCore(intr_worker, "intr_worker", WORKER_STACK,
                                             NULL, WORKER_PRIO, &s_worker, INTR_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    /* ---- 5. 使能并启动定时器 ---- */
    err = gptimer_enable(s_timer);
    err |= gptimer_start(s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动定时器失败: 0x%x", err);
        return;
    }

    /* ---- 6. 观察 RUN_SECONDS 秒 ---- */
    vTaskDelay(pdMS_TO_TICKS(RUN_SECONDS * 1000));

    /* ---- 7. 立刻停表：先停表再等任务退出，保证 ISR 计数窗口 = RUN_SECONDS ---- */
    gptimer_stop(s_timer);
    gptimer_disable(s_timer);

    /* 给 worker 一点时间走完当前循环并退出（它自己也会到 deadline 退出） */
    vTaskDelay(pdMS_TO_TICKS(300));

    int64_t avg_us = (s_wake_cnt > 0) ? (s_lat_sum_us / (int64_t)s_wake_cnt) : 0;
    uint32_t expected_isr = RUN_SECONDS * 1000000u / ISR_PERIOD_US;

    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "ISR 次数 %u（理论 %u）| 任务唤醒 %u | 任务等待超时 %u",
             (unsigned)s_isr_cnt, (unsigned)expected_isr,
             (unsigned)s_wake_cnt, (unsigned)s_timeout_cnt);
    ESP_LOGI(TAG, "唤醒延迟 最小 %lld / 平均 %lld / 最大 %lld us",
             (long long)s_lat_min_us, (long long)avg_us, (long long)s_lat_max_us);

    gptimer_del_timer(s_timer);
    s_timer = NULL;

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
