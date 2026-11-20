/*
 * crash_demo.c —— 故意崩溃，用来练 GDB（D1 第一课）
 *
 * 为什么故意造崩溃：调试器最大的价值就是"从崩溃现场还原真相"。
 * 这里造一个有 3 层调用栈的崩溃，方便你练习：
 *   bt         看调用栈（能看出是怎么一层层调下来的）
 *   info reg   看寄存器（MEPC 指向出错指令，SP 指向栈）
 *   info threads  看所有任务（哪个任务在跑、栈在哪）
 *   frame N    切到某一层，看那一层的局部变量
 *
 * 注意：开了 CONFIG_ESP_SYSTEM_PANIC_GDBSTUB 之后，崩溃不会重启，
 * 而是停住等 GDB 连接。断开后需要手动复位。
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "crash_demo.h"

static const char *TAG = "CRASH";

/* 三级调用：让调用栈有层次，方便练 frame 切换 */
static void crash_level_3(void)
{
    ESP_LOGI(TAG, "进入第 3 层，准备往空指针写数据");
    volatile int *null_ptr = (volatile int *)0x0;
    *null_ptr = 0x1234;          /* ← 这里会触发 Store access fault */
    ESP_LOGE(TAG, "这一行不应该被执行");
}

static void crash_level_2(void)
{
    crash_level_3();
}

static void crash_level_1(void)
{
    crash_level_2();
}

void crash_demo_start(void)
{
    ESP_LOGI(TAG, "==== 崩溃调试实验 ====");
    ESP_LOGI(TAG, "3 秒后会故意崩溃，CPU 将停在 GDB Stub 等待连接");
    ESP_LOGI(TAG, "连上后试：bt / info registers / info threads / frame 1");

    vTaskDelay(pdMS_TO_TICKS(3000));

    crash_level_1();

    /* 走到这里说明没崩（不应该发生） */
    ESP_LOGE(TAG, "竟然没崩？检查 sdkconfig 的 PANIC_GDBSTUB 配置");
}
