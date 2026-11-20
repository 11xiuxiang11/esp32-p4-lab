/*
 * fault_demo.c —— 四类典型故障（D3，对应"你遇到过什么难题"的面试素材）
 *
 * 每个故障都按同一套路记录：现象 → 定位手段 → 根因 → 修法（见 docs/07-造故障与定位.md）
 *
 * 模式（改 FAULT_MODE）：
 *   0 栈溢出   ：任务栈 1KB，递归 40 层 → 栈写穿 → canary 检测到 → panic
 *   1 堆耗尽   ：连续 malloc 1KB 直到失败 → 再创建 32KB 栈的任务 → 创建失败
 *   2 看门狗   ：两个任务同核各忙 60ms/100ms（总 120%）→ IDLE0 饿死 → task_wdt
 *   3 死锁     ：任务 A 先拿 m1；任务 B 先拿 m2；再互相等 → 双方永久阻塞
 *
 * 注意：
 *   - 模式 0 的 panic 默认会停在 GDB Stub（CONFIG_ESP_SYSTEM_PANIC_GDBSTUB），不会重启
 *   - 模式 2 的 WDT 每 5 秒报一次，不重启（PANIC 未开）
 *   - 模式 3 没有任何报错——这正是死锁最难查的地方
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "fault_demo.h"

static const char *TAG = "FAULT";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define FAULT_STACK_OVERFLOW   0
#define FAULT_HEAP_EXHAUST     1
#define FAULT_WATCHDOG         2
#define FAULT_DEADLOCK         3

#define FAULT_MODE             3   /* ← 改这一行 */

/*============================================================================
 * ② 公共参数
 *============================================================================*/
#define FAULT_CORE             0
#define MAIN_PRIO_BOOST        7
#define OBSERVE_MS             8000

/*============================================================================
 * ③ 模式 0：栈溢出
 *============================================================================*/
#if FAULT_MODE == FAULT_STACK_OVERFLOW

#define SMALL_STACK_BYTES      8192u    /* 修复后：8KB */
#define RECURSION_DEPTH        40u      /* 每层约 100 字节 → 远超 1KB */

/* 每层栈帧带 96 字节局部数组，让溢出来得快一点 */
static uint32_t recurse(uint32_t depth)
{
    volatile uint8_t frame[96];
    frame[0]  = (uint8_t)depth;
    frame[95] = (uint8_t)(depth ^ 0x5A);

    if (depth == 0) {
        return (uint32_t)frame[0] + frame[95];
    }
    return recurse(depth - 1) + frame[0];
}

static void overflow_task(void *arg)
{
    ESP_LOGI(TAG, "栈溢出任务启动：栈 %u 字节，递归 %u 层",
             SMALL_STACK_BYTES, RECURSION_DEPTH);
    vTaskDelay(pdMS_TO_TICKS(500));   /* 让日志先打出来 */

    volatile uint32_t r = recurse(RECURSION_DEPTH);

    /* 验证手段：栈的历史最低剩余（字节）。剩得越多越安全。 */
    UBaseType_t remain = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "没崩：r=%u，栈 %u 字节，最少剩 %u 字节（用了约 %u）",
             (unsigned)r, SMALL_STACK_BYTES, (unsigned)remain,
             (unsigned)(SMALL_STACK_BYTES - remain));
    vTaskDelete(NULL);
}

void fault_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 0：栈溢出 ====");
    ESP_LOGI(TAG, "预期：打印 stack overflow 并 panic（看门狗/日志里能看到任务名）");

    UBaseType_t saved = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);
    xTaskCreatePinnedToCore(overflow_task, "overflow", SMALL_STACK_BYTES, NULL,
                            5, NULL, FAULT_CORE);
    vTaskPrioritySet(NULL, saved);

    vTaskDelay(pdMS_TO_TICKS(OBSERVE_MS));
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* 栈溢出 */

/*============================================================================
 * ④ 模式 1：堆耗尽
 *============================================================================*/
#if FAULT_MODE == FAULT_HEAP_EXHAUST

#define BLOCK_BYTES            1024u
#define MAX_BLOCKS             2048
#define NEW_TASK_STACK         32768u

/* 空任务：只用来验证能不能创建出 32KB 栈的任务 */
static void big_stack_task(void *arg)
{
    vTaskDelete(NULL);
}

static void heap_exhaust_start(void)
{
    ESP_LOGI(TAG, "==== 模式 1：堆耗尽 ====");
    ESP_LOGI(TAG, "开始前：空闲 %u B，最大连续块 %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* 2048 个指针 = 16KB，放静态区；放栈上会先把 main 任务的栈撑爆 */
    static void *blocks[MAX_BLOCKS];
    int count = 0;

    /* 一直申请，直到失败 */
    for (int i = 0; i < MAX_BLOCKS; i++) {
        blocks[i] = heap_caps_malloc(BLOCK_BYTES, MALLOC_CAP_INTERNAL);
        if (blocks[i] == NULL) {
            ESP_LOGW(TAG, "第 %d 块（%u B）分配失败", i, BLOCK_BYTES);
            break;
        }
        count++;
    }

    ESP_LOGI(TAG, "共分配 %d 块 × %u B；当前空闲 %u B，最大连续块 %u B",
             count, BLOCK_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* 此时想创建一个需要 32KB 栈的任务 —— 会发生什么？ */
    BaseType_t ret = xTaskCreatePinnedToCore(big_stack_task, "big",
                                             NEW_TASK_STACK, NULL, 5, NULL, FAULT_CORE);
    ESP_LOGI(TAG, "创建 32KB 栈的任务：%s", ret == pdPASS ? "成功" : "失败（内存不足）");

    /* 回收 */
    for (int i = 0; i < count; i++) {
        heap_caps_free(blocks[i]);
    }
    ESP_LOGI(TAG, "释放全部后：空闲 %u B，最大连续块 %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

void fault_demo_start(void)
{
    heap_exhaust_start();
}

#endif /* 堆耗尽 */

/*============================================================================
 * ⑤ 模式 2：看门狗（饿死 IDLE0）
 *============================================================================*/
#if FAULT_MODE == FAULT_WATCHDOG

#define WD_BUSY_MS             120      /* 比周期还长：任务根本没机会睡 */
#define WD_PERIOD_MS           100      /* 周期 100ms */
#define WD_TASK_PRIO           5

/* 墙钟忙等：被抢占也照样退出（所以周期不变，但 IDLE 被饿死） */
static void wd_busy_ms(uint32_t ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < (int64_t)ms * 1000) {
    }
}

static void wd_task(void *arg)
{
    const char *name = (const char *)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 0;

    while (1) {
        wd_busy_ms(WD_BUSY_MS);
        n++;
        if ((n % 20) == 0) {
            ESP_LOGI(TAG, "%s 已跑 %u 轮", name, (unsigned)n);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(WD_PERIOD_MS));
    }
}

void fault_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 2：看门狗 ====");
    ESP_LOGI(TAG, "两个任务同核、各忙 %u ms（周期 %u ms）→ 忙等超过周期，IDLE 必定饿死 → 5 秒后 task_wdt",
             WD_BUSY_MS, WD_PERIOD_MS);

    UBaseType_t saved = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);
    xTaskCreatePinnedToCore(wd_task, "wd_a", 4096, (void *)"WD_A", WD_TASK_PRIO, NULL, FAULT_CORE);
    xTaskCreatePinnedToCore(wd_task, "wd_b", 4096, (void *)"WD_B", WD_TASK_PRIO, NULL, FAULT_CORE);
    vTaskPrioritySet(NULL, saved);

    vTaskDelay(pdMS_TO_TICKS(OBSERVE_MS));
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* 看门狗 */

/*============================================================================
 * ⑥ 模式 3：死锁（两把锁交叉）
 *============================================================================*/
#if FAULT_MODE == FAULT_DEADLOCK

static SemaphoreHandle_t s_m1 = NULL;
static SemaphoreHandle_t s_m2 = NULL;

static void deadlock_a(void *arg)
{
    ESP_LOGI(TAG, "A：先拿 m1");
    xSemaphoreTake(s_m1, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(200));      /* 给 B 时间去拿 m2 */

    ESP_LOGI(TAG, "A：再拿 m2（如果卡住就是死锁）");
   if(xSemaphoreTake(s_m2, 20)) {
        ESP_LOGI(TAG, "A：拿到 m2");
        xSemaphoreGive(s_m2);
        xSemaphoreGive(s_m1);
    } else {
        ESP_LOGW(TAG, "A：拿 m2 超时（死锁）");
        xSemaphoreGive(s_m1);
    }

    ESP_LOGI(TAG, "A：拿到两把锁（不会执行到）");
   
    
    vTaskDelete(NULL);
}

static void deadlock_b(void *arg)
{
    ESP_LOGI(TAG, "B：先拿 m2");
    xSemaphoreTake(s_m2, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "B：再拿 m1（如果卡住就是死锁）");
    if(xSemaphoreTake(s_m1, 30)) {
        ESP_LOGI(TAG, "B：拿到 m1");
        xSemaphoreGive(s_m1);
        xSemaphoreGive(s_m2);
    } else {
        ESP_LOGW(TAG, "B：拿 m1 超时（死锁）");
        xSemaphoreGive(s_m2);
    }

    ESP_LOGI(TAG, "B：拿到两把锁（不会执行到）");
 
   
    vTaskDelete(NULL);
}

void fault_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 3：死锁 ====");
    ESP_LOGI(TAG, "A 按 m1→m2 顺序，B 按 m2→m1 顺序；各自拿一把后再等对方 → 永久阻塞");
    ESP_LOGI(TAG, "注意：不会有任何报错，2 秒后 main 打印『系统静止』");

    s_m1 = xSemaphoreCreateMutex();
    s_m2 = xSemaphoreCreateMutex();
    if (s_m1 == NULL || s_m2 == NULL) {
        ESP_LOGE(TAG, "创建互斥量失败");
        return;
    }

    UBaseType_t saved = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);
    xTaskCreatePinnedToCore(deadlock_a, "dl_a", 4096, NULL, 5, NULL, FAULT_CORE);
    xTaskCreatePinnedToCore(deadlock_b, "dl_b", 4096, NULL, 5, NULL, FAULT_CORE);
    vTaskPrioritySet(NULL, saved);

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 此时两个任务应该都阻塞在 take 上；IDLE 还在跑，所以不会有 WDT */
    ESP_LOGW(TAG, "如果上面没有出现『拿到两把锁』，就是死锁了：系统静止但无报错");
    ESP_LOGI(TAG, "排查方法：调试器里 info threads 看两个任务都停在 xQueueSemaphoreTake");
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* 死锁 */
