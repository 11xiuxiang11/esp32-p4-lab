/*
 * event_timer_demo.c —— 事件组 + 软件定时器实验（B5，对应视频 13-14）
 *
 * 两个模式，改顶部 ET_MODE 切换：
 *
 *   0 = 事件组门控
 *       三个"初始化"任务分别在 200ms / 300ms / 800ms 后置自己的位
 *       采集任务用 xEventGroupWaitBits 等三位全满足（clear_on_exit = pdTRUE）
 *       预期：约 800ms 后开工，位图 = 0b111
 *
 *   1 = 软件定时器
 *       ① 周期定时器 100ms：统计实际间隔的 最小/最大/平均
 *       ② 单次定时器 2000ms + 每 300ms 调一次 xTimerReset（模拟"一直按着按键"）
 *          续命 5 次（1.5 秒）后停止，2 秒后到期 → 约 3.5 秒触发
 *       ③ 一个"干扰任务"每 400ms 忙 250ms（优先级 5）
 *          因为守护任务优先级只有 1，会被它推迟 → 定时器抖动可见
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "event_timer_demo.h"

static const char *TAG = "EVTMR";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define ET_MODE_GATE    0
#define ET_MODE_TIMER   1

#define ET_MODE         ET_MODE_TIMER     /* ← 改这一行切换实验 */

/*============================================================================
 * ② 公共参数
 *============================================================================*/
#define TASK_STACK_BYTES    4096u
#define ET_CORE             0
#define MAIN_PRIO_BOOST     7
#define INIT_PRIO           4       /* 初始化任务 / 续命任务的优先级 */

/*============================================================================
 * ③ 模式 0：事件组门控
 *============================================================================*/
#if ET_MODE == ET_MODE_GATE

#define BIT_WIFI_OK         (1u << 0)
#define BIT_SRV_OK          (1u << 1)
#define BIT_SENSOR_OK       (1u << 2)
#define BITS_ALL            (BIT_WIFI_OK | BIT_SRV_OK | BIT_SENSOR_OK)

#define WIFI_DELAY_MS       200
#define SENSOR_DELAY_MS     300
#define SRV_DELAY_MS        800

#define SAMPLER_PRIO        5

static EventGroupHandle_t s_eg = NULL;
static int64_t s_boot_us = 0;

/* 三个初始化任务：干完活置自己那一位 */
static void init_wifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_DELAY_MS));
    xEventGroupSetBits(s_eg, BIT_WIFI_OK);
    ESP_LOGI(TAG, "Wi-Fi 就绪（%d ms）", WIFI_DELAY_MS);
    vTaskDelete(NULL);
}

static void init_sensor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SENSOR_DELAY_MS));
    xEventGroupSetBits(s_eg, BIT_SENSOR_OK);
    ESP_LOGI(TAG, "传感器就绪（%d ms）", SENSOR_DELAY_MS);
    vTaskDelete(NULL);
}

static void init_server_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SRV_DELAY_MS));
    xEventGroupSetBits(s_eg, BIT_SRV_OK);
    ESP_LOGI(TAG, "服务器握手完成（%d ms）", SRV_DELAY_MS);
    vTaskDelete(NULL);
}

/* 采集任务：三个条件全满足才开工 */
static void sampler_task(void *arg)
{
    EventBits_t got = xEventGroupWaitBits(s_eg, BITS_ALL,
                                          pdTRUE,          /* 等到后清零 */
                                          pdTRUE,          /* 三个都要满足 */
                                          portMAX_DELAY);

    int64_t elapsed_ms = (esp_timer_get_time() - s_boot_us) / 1000;
    ESP_LOGI(TAG, "门控打开！位图 = 0b%03b（十进制 %u），从启动到开工 %lld ms",
             (unsigned)got, (unsigned)got, (long long)elapsed_ms);

    vTaskDelete(NULL);
}

#endif /* ET_MODE_GATE */

/*============================================================================
 * ④ 模式 1：软件定时器
 *============================================================================*/
#if ET_MODE == ET_MODE_TIMER

#define TICK_PERIOD_MS      100         /* 周期定时器 */
#define LONG_PERIOD_MS      2000        /* 单次定时器（长按阈值） */
#define REHOLD_TIMES        5           /* 续命次数 */
#define REHOLD_STEP_MS      300         /* 每次续命的间隔 */
#define JITTER_RUN_MS       5000        /* 总共观察多久 */

#define JAMMER_PERIOD_MS    400         /* 干扰任务周期 */
#define JAMMER_BUSY_MS      250         /* 干扰任务每次占用 CPU 多久（CPU 时间） */
#define JAMMER_PRIO         5

/* 周期定时器的统计 */
static volatile uint32_t s_tick_cnt   = 0;
static volatile int64_t  s_sum_ms     = 0;
static volatile int64_t  s_min_ms     = 1000000;
static volatile int64_t  s_max_ms     = 0;
static volatile int64_t  s_last_tick_us = 0;

static volatile uint32_t s_long_fired = 0;

/* CPU 时间型忙等：需要标定（同 B2-2 的做法） */
static volatile uint32_t s_cpu_sink;
static uint32_t s_units_per_ms = 1;

static void cpu_work(uint32_t iters)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += i * 2654435761u;
        acc ^= acc >> 7;
    }
    s_cpu_sink = acc;
}

static uint32_t calibrate_units_per_ms(void)
{
    const uint32_t probe = 20000;
    int64_t t0 = esp_timer_get_time();
    cpu_work(probe);
    int64_t us = esp_timer_get_time() - t0;
    if (us <= 0) {
        return 1;
    }
    return (uint32_t)(((uint64_t)probe * 1000u) / (uint64_t)us);
}

/* 周期定时器回调：记录实际间隔。
 * 注意：这里为了演示打了几行日志；真实工程回调里不要打印。 */
static void tick_cb(TimerHandle_t t)
{
    (void)t;
    int64_t now_us = esp_timer_get_time();

    if (s_last_tick_us != 0) {
        int64_t d_ms = (now_us - s_last_tick_us) / 1000;
        s_sum_ms += d_ms;
        s_tick_cnt++;
        if (d_ms < s_min_ms) {
            s_min_ms = d_ms;
        }
        if (d_ms > s_max_ms) {
            s_max_ms = d_ms;
        }
        if ((s_tick_cnt % 10) == 0) {
            ESP_LOGI(TAG, "第 %u 次 tick，本次间隔 %lld ms",
                     (unsigned)s_tick_cnt, (long long)d_ms);
        }
    }
    s_last_tick_us = now_us;
}

/* 单次定时器回调：长按触发 */
static void long_press_cb(TimerHandle_t t)
{
    (void)t;
    s_long_fired++;
    ESP_LOGI(TAG, ">>> 长按触发！<<<");
}

/* 模拟"一直按着按键"：每 300ms 续命一次，共 5 次 */
static void rehold_task(void *arg)
{
    for (int i = 1; i <= REHOLD_TIMES; i++) {
        vTaskDelay(pdMS_TO_TICKS(REHOLD_STEP_MS));
        BaseType_t ok = xTimerReset((TimerHandle_t)arg, 0);
        ESP_LOGI(TAG, "第 %d 次续命（xTimerReset 返回 %s）",
                 i, ok == pdPASS ? "pdPASS" : "pdFAIL");
    }
    ESP_LOGI(TAG, "停止续命，%d ms 后应触发长按", LONG_PERIOD_MS);
    vTaskDelete(NULL);
}

/* 干扰任务：制造 CPU 忙，验证"守护任务优先级 1 会被推迟" */
static void jammer_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    int64_t deadline = esp_timer_get_time() + (int64_t)JITTER_RUN_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        cpu_work(s_units_per_ms * JAMMER_BUSY_MS);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(JAMMER_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

#endif /* ET_MODE_TIMER */

/*============================================================================
 * ⑤ 对外入口
 *============================================================================*/
void event_timer_demo_start(void)
{
#if ET_MODE == ET_MODE_GATE
    ESP_LOGI(TAG, "==== 模式 0：事件组门控 ====");
    ESP_LOGI(TAG, "三个初始化分别需要 %d / %d / %d ms，采集任务等三个条件都满足",
             WIFI_DELAY_MS, SENSOR_DELAY_MS, SRV_DELAY_MS);

    s_eg = xEventGroupCreate();
    if (s_eg == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }

    s_boot_us = esp_timer_get_time();

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(sampler_task, "sampler", TASK_STACK_BYTES, NULL,
                                   SAMPLER_PRIO, NULL, ET_CORE);
    ret |= xTaskCreatePinnedToCore(init_wifi_task, "wifi", TASK_STACK_BYTES, NULL,
                                   INIT_PRIO, NULL, ET_CORE);
    ret |= xTaskCreatePinnedToCore(init_sensor_task, "sensor", TASK_STACK_BYTES, NULL,
                                   INIT_PRIO, NULL, ET_CORE);
    ret |= xTaskCreatePinnedToCore(init_server_task, "server", TASK_STACK_BYTES, NULL,
                                   INIT_PRIO, NULL, ET_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    /* 再看一次位图：因为 sampler 用了 clear_on_exit，这里应该是 0b000 */
    ESP_LOGI(TAG, "结束后当前位图 = 0b%03b（clear_on_exit 已把位清掉）",
             (unsigned)xEventGroupGetBits(s_eg));

    vEventGroupDelete(s_eg);
    s_eg = NULL;

#else /* ET_MODE_TIMER */
    ESP_LOGI(TAG, "==== 模式 1：软件定时器 ====");
    ESP_LOGI(TAG, "周期定时器 %d ms；单次定时器 %d ms，续命 %d 次 × %d ms；观察 %d ms",
             TICK_PERIOD_MS, LONG_PERIOD_MS, REHOLD_TIMES, REHOLD_STEP_MS, JITTER_RUN_MS);

    s_units_per_ms = calibrate_units_per_ms();
    ESP_LOGI(TAG, "标定：1ms CPU ≈ %u 次迭代", (unsigned)s_units_per_ms);

    TimerHandle_t t_tick = xTimerCreate("tick", pdMS_TO_TICKS(TICK_PERIOD_MS),
                                        pdTRUE, NULL, tick_cb);
    TimerHandle_t t_long = xTimerCreate("long", pdMS_TO_TICKS(LONG_PERIOD_MS),
                                        pdFALSE, NULL, long_press_cb);
    if (t_tick == NULL || t_long == NULL) {
        ESP_LOGE(TAG, "创建定时器失败（命令队列或内存不足）");
        return;
    }

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(rehold_task, "rehold", TASK_STACK_BYTES, t_long,
                                   INIT_PRIO, NULL, ET_CORE);
    ret |= xTaskCreatePinnedToCore(jammer_task, "jammer", TASK_STACK_BYTES, NULL,
                                   JAMMER_PRIO, NULL, ET_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    /* 启动定时器：第二个参数是"命令入队等待时间"，不是周期！ */
    if (xTimerStart(t_tick, pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGE(TAG, "tick 定时器启动命令投递失败");
    }
    if (xTimerStart(t_long, pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGE(TAG, "long 定时器启动命令投递失败");
    }

    vTaskDelay(pdMS_TO_TICKS(JITTER_RUN_MS));

    int64_t avg_ms = (s_tick_cnt > 0) ? (s_sum_ms / (int64_t)s_tick_cnt) : 0;
    ESP_LOGI(TAG, "==== 结果 ====");
    ESP_LOGI(TAG, "tick 回调次数 %u | 间隔 最小 %lld / 平均 %lld / 最大 %lld ms（设定 %d ms）",
             (unsigned)s_tick_cnt, (long long)s_min_ms, (long long)avg_ms,
             (long long)s_max_ms, TICK_PERIOD_MS);
    ESP_LOGI(TAG, "长按触发次数 %u（预期 1 次）", (unsigned)s_long_fired);
    ESP_LOGI(TAG, "说明：最大间隔明显大于 %d ms，就是守护任务（优先级 1）被干扰任务推迟的证据",
             TICK_PERIOD_MS);

    (void)xTimerDelete(t_tick, pdMS_TO_TICKS(10));
    (void)xTimerDelete(t_long, pdMS_TO_TICKS(10));
#endif

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
