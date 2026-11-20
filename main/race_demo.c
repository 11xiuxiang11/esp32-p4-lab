/*
 * race_demo.c —— 数据竞争最小演示（B2 · 同步与互斥）
 *
 * 一次跑两轮对照，用来说明"竞态窗口有多大"决定了它多久暴露一次：
 *   第 1 轮：紧循环 g_counter++          窗口只有 2~3 个指令周期，可能一次都不丢
 *   第 2 轮：宽窗口（读 → 空转 → 写）     窗口被拉宽，几乎必然丢
 *
 * 你的任务：把 TODO-1 / TODO-2 两处改成正确写法（临界区 或 原子操作），
 *          让两轮的丢失都变成 0。
 *
 * 注意：任务函数里【永远不能 return】——返回会触发内核断言并重启板子。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "race_demo.h"

static const char *TAG = "RACE";
static portMUX_TYPE s_counter_mux = portMUX_INITIALIZER_UNLOCKED;
/* ---- 参数（规范第 2 条：不许魔数）---- */
#define RACE_INC_TIMES      100000u   /* 每个任务加多少次 */
#define RACE_ROUND_WAIT_MS  500       /* 每轮等多久再判定 */
#define RACE_TASK_STACK     4096u     /* 字节 */
#define RACE_TASK_PRIO      5
#define RACE_TASK_A_CORE    0
#define RACE_TASK_B_CORE    1
#define RACE_WIDE_LOOPS     32u       /* 宽窗口里空转多少圈（越大越容易撞） */

/* 共享变量：加 volatile 是为了强制每次真去内存读，
 * 但 volatile 本身【不能】防止丢更新 —— 这正是本节的要点。 */
static volatile uint32_t g_counter = 0;

/* 当前这一轮用哪种窗口 */
static volatile bool s_use_wide_window = false;

static void race_task(void *arg)
{
    const char *task_name = (const char *)arg;

    for (uint32_t i = 0; i < RACE_INC_TIMES; i++) {
        if (s_use_wide_window) {
            /* 宽窗口：把"读"和"写"之间拉开几十个周期 */
            portENTER_CRITICAL(&s_counter_mux);
            uint32_t tmp = g_counter;              /* 读 */
            for (volatile uint32_t k = 0; k < RACE_WIDE_LOOPS; k++) {
                /* 空转，制造竞态窗口 */
            }
            /* TODO-2：宽窗口版的数据竞争现场，改法和 TODO-1 一样 */
            g_counter = tmp + 1;                   /* 写回 */
            portEXIT_CRITICAL(&s_counter_mux);
        } else {
            /* TODO-1：紧循环版的数据竞争现场。
             * 修法二选一：
             *   (a) 临界区：portENTER_CRITICAL(&mux) ... portEXIT_CRITICAL(&mux)
             *       （需要一个全局 portMUX_TYPE，两个任务必须用同一个）
             *   (b) 原子操作：__atomic_fetch_add(&g_counter, 1, __ATOMIC_SEQ_CST)
             * 临界区里绝对不能放 ESP_LOGI / printf / vTaskDelay。
             */
            portENTER_CRITICAL(&s_counter_mux);
            g_counter++;
            portEXIT_CRITICAL(&s_counter_mux);
        }
    }

    ESP_LOGI(TAG, "任务 %s 完成", task_name);

    /* 铁律：任务函数永远不许 return，干完活自己删自己 */
    vTaskDelete(NULL);
}

/* 跑一轮实验：清零 → 建两个任务 → 等 → 打印结果 */
static void run_one_round(const char *round_name, bool wide_window)
{
    s_use_wide_window = wide_window;
    g_counter = 0;

    BaseType_t ret;

    /* 【关键】创建期间把 main 任务提到比 worker 更高：
     * 否则建完 race0 之后，race0（优先级 5）会立刻抢占 main（优先级 1），
     * main 根本轮不到去创建 race1 → 两个任务变成一前一后串行，永远撞不上。
     * 这是"我以为并行、其实串行"的经典翻车点。
     */
    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, RACE_TASK_PRIO + 1);

    ret = xTaskCreatePinnedToCore(race_task, "race0", RACE_TASK_STACK,
                                  (void *)"race0", RACE_TASK_PRIO, NULL,
                                  RACE_TASK_A_CORE);
    ret |= xTaskCreatePinnedToCore(race_task, "race1", RACE_TASK_STACK,
                                   (void *)"race1", RACE_TASK_PRIO, NULL,
                                   RACE_TASK_B_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "[%s] 创建任务失败", round_name);
        vTaskPrioritySet(NULL, saved_prio);
        return;
    }

    /* 土办法等：睡够时间。缺点——慢机器可能没算完，快机器白等。
     * 正确做法是让两个任务做完后各 give 一次信号量，主任务 take 两次（B3 讲）。 */
    vTaskDelay(pdMS_TO_TICKS(RACE_ROUND_WAIT_MS));

    vTaskPrioritySet(NULL, saved_prio);

    uint32_t expected = 2u * RACE_INC_TIMES;
    uint32_t actual   = g_counter;
    uint32_t lost     = (actual <= expected) ? (expected - actual) : 0;

    ESP_LOGI(TAG, "[%s] 期望 %u | 实际 %u | 丢失 %u",
             round_name, (unsigned)expected, (unsigned)actual, (unsigned)lost);
}

/* 对外入口 */
void race_demo_start(void)
{
    ESP_LOGI(TAG, "==== 数据竞争实验：两轮对照 ====");

    run_one_round("紧循环", false);
    run_one_round("宽窗口", true);

    ESP_LOGI(TAG, "==== 实验结束 ====");
}
