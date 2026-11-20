/*
 * priority_inversion.c —— 优先级翻转演示（B2-2）
 *
 * 三个任务都绑在 Core0（必须在同一个核，抢占才看得出来）：
 *   L  优先级 2：拿到互斥量 → 做 300ms 的活 → 放锁
 *   M  优先级 4：不碰锁，纯做 200ms 的活        ← 它是"坏人"
 *   H  优先级 6：要锁，记录自己等了多久
 *
 * 预期（实测修正版！）：
 *   互斥量 + 有 M  → H 等 ≈ 270ms，L 放锁前优先级显示 6   ← 继承挡住 M，翻转被消除
 *   互斥量 + 无 M  → H 等 ≈ 270ms，L 放锁前优先级【仍是 6】← 继承照样触发，只是没人来抢
 *   二值信号量 + 有 M → H 等 ≈ 470ms，L 优先级是 2        ← 无继承，翻转现形
 *
 * 重要区分（容易搞混）：
 *   · 继承靠什么触发：只要有【更高优先级任务在等这把锁】，holder 就被提升。
 *     H 全程都在等锁 → 只要用互斥量，L 一定被提到 6，与有没有 M 无关。
 *   · 继承有什么用：只有【有中优先级任务想插队】时才看得出效果。
 *     所以"有没有 M"决定的是"现象看不看得见"，不是"继承触不触发"。
 *
 * 注意：互斥量默认【带】优先级继承（configUSE_MUTEXES=1），
 *       所以想看到翻转必须换成二值信号量（USE_MUTEX_LOCK=0）。
 *
 * 两轮对比就能看到"高优先级被中优先级拖住"的完整现象。
 *
 * 注意几个必须做到的细节：
 *   1. 三个任务必须绑同一个核
 *   2. L 的"300ms 活"必须按【CPU 时间】算（不能用墙钟），
 *      否则被抢占也照样 300ms 结束，就看不到持锁时间被拉长
 *   3. 初始延时错开，保证 L 先拿到锁、H 再来要锁、M 最后插队
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "priority_inversion.h"
#include "esp_timer.h"

static const char *TAG = "INV";

/*============================================================================
 * 实验参数
 *============================================================================*/
#define INCLUDE_MID_TASK        0       /* 1 = 有 M（看翻转）；0 = 无 M（对照） */
#define USE_MUTEX_LOCK          1       /* 1 = 互斥量（有继承）；0 = 二值信号量（无继承，看翻转） */
#define INV_CORE                0       /* 三个任务都绑这个核 */

#define PRIO_L                  2
#define PRIO_M                  4
#define PRIO_H                  6
#define MAIN_PRIO_BOOST         7       /* 创建期间 main 临时提到最高 */

#define L_HOLD_MS               300     /* L 持锁期间要做的活（CPU 时间） */
#define M_BUSY_MS               200     /* M 纯计算的时间（CPU 时间） */
#define H_START_DELAY_MS        30      /* H 稍晚再要锁，保证锁已经在 L 手里 */
#define M_START_DELAY_MS        50      /* M 再晚一点醒来，插到 L 和 H 中间 */
#define TASK_STACK_BYTES        4096u
#define OBSERVE_MS              2000    /* main 等多久后收工 */

/*============================================================================
 * 按 CPU 时间计算的忙等（这是本实验能看到翻转的关键）
 *============================================================================*/
static volatile uint32_t s_cpu_sink;
static uint32_t s_units_per_ms = 1;

static void cpu_work(uint32_t iterations)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        acc += i * 2654435761u;
        acc ^= acc >> 7;
    }
    s_cpu_sink = acc;
}

static uint32_t calibrate_units_per_ms(void)
{
    const uint32_t probe_units = 20000;
    int64_t t0 = esp_timer_get_time();
    cpu_work(probe_units);
    int64_t elapsed_us = esp_timer_get_time() - t0;
    if (elapsed_us <= 0) {
        return 1;
    }
    return (uint32_t)(((uint64_t)probe_units * 1000u) / (uint64_t)elapsed_us);
}

/* 干 ms 毫秒的"活"：按计算量算，被抢占就会拖长 */
static void do_cpu_work_ms(uint32_t ms)
{
    cpu_work(s_units_per_ms * ms);
}

/*============================================================================
 * 三个任务
 *============================================================================*/
static SemaphoreHandle_t s_mutex = NULL;

static void task_l(void *arg)
{
    ESP_LOGI(TAG, "L(2) 启动，准备拿锁");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "L(2) 拿到锁，开始做 %u ms 的活", L_HOLD_MS);

    int64_t t_start = esp_timer_get_time();
    do_cpu_work_ms(L_HOLD_MS);
    int64_t held_ms = (esp_timer_get_time() - t_start) / 1000;

    /* 【这就是优先级继承的铁证】
     * L 自己没有改过优先级，但此刻若 H 正在等这把锁，
     * L 会被内核临时提升到 H 的优先级（6）。放锁之后才恢复成 2。 */
    ESP_LOGI(TAG, "L 放锁前：我的优先级 = %d（原始 %d，继承生效时会是 %d）",
             (int)uxTaskPriorityGet(NULL), PRIO_L, PRIO_H);

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "L(2) 放锁。实际持锁 %lld ms（理想 %u ms）",
             (long long)held_ms, L_HOLD_MS);

    vTaskDelete(NULL);
}

#if INCLUDE_MID_TASK
static void task_m(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(M_START_DELAY_MS));

    ESP_LOGI(TAG, "M(4) 醒来，纯算 %u ms（不碰锁）", M_BUSY_MS);
    int64_t t_start = esp_timer_get_time();
    do_cpu_work_ms(M_BUSY_MS);
    ESP_LOGI(TAG, "M(4) 算完，用了 %lld ms",
             (long long)((esp_timer_get_time() - t_start) / 1000));

    vTaskDelete(NULL);
}
#endif /* INCLUDE_MID_TASK */

static void task_h(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(H_START_DELAY_MS));

    ESP_LOGI(TAG, "H(6) 来要锁");
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t waited_ms = (esp_timer_get_time() - t0) / 1000;

    ESP_LOGI(TAG, "H(6) 拿到锁，等了 %lld ms", (long long)waited_ms);

    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

/*============================================================================
 * 统一入口
 *============================================================================*/
void priority_inversion_start(void)
{
    ESP_LOGI(TAG, "==== 优先级翻转实验 ====");

    s_units_per_ms = calibrate_units_per_ms();
    ESP_LOGI(TAG, "标定：1ms CPU ≈ %u 次迭代", (unsigned)s_units_per_ms);
    ESP_LOGI(TAG, "配置：含 M = %s，锁类型 = %s",
             INCLUDE_MID_TASK ? "是" : "否",
             USE_MUTEX_LOCK ? "互斥量(有优先级继承)" : "二值信号量(无继承)");

#if USE_MUTEX_LOCK
    s_mutex = xSemaphoreCreateMutex();
#else
    /* 二值信号量创建后是【空的】，必须先 give 一次才能当锁用。
     * 它没有"主人"概念 → 内核不会做优先级继承 → 用来看翻转 */
    s_mutex = xSemaphoreCreateBinary();
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
#endif
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "创建锁失败");
        return;
    }

    /* 创建期间把 main 提到最高，防止被刚创建的任务抢占导致后面的任务建不出来 */
    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    BaseType_t ret = pdPASS;
    ret |= xTaskCreatePinnedToCore(task_l, "L", TASK_STACK_BYTES, NULL,
                                   PRIO_L, NULL, INV_CORE);
#if INCLUDE_MID_TASK
    ret |= xTaskCreatePinnedToCore(task_m, "M", TASK_STACK_BYTES, NULL,
                                   PRIO_M, NULL, INV_CORE);
#endif
    ret |= xTaskCreatePinnedToCore(task_h, "H", TASK_STACK_BYTES, NULL,
                                   PRIO_H, NULL, INV_CORE);

    vTaskPrioritySet(NULL, saved_prio);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(OBSERVE_MS));
    ESP_LOGI(TAG, "==== 实验结束 ====");
}
