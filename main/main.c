/*
 * L2 · 任务与调度 —— 最小可烧实验
 *
 * 这个实验只做三件事，用来让你"看见"调度器：
 *   1. 两个 worker 任务按固定周期干活，其中一段是"死占 CPU"的忙等
 *   2. 第三个 monitor 任务每 5 秒打一次运行时间统计 + 栈水位
 *   3. 通过改下面的宏，观察 同核同优先级 / 同核不同优先级 / 双核 三种现象
 *
 * 关键概念对照代码：
 *   xTaskCreatePinnedToCore 的最后一个参数 = 绑定到哪个核（0 或 1）
 *   xTaskCreatePinnedToCore 的第 4 个参数 = 传给任务函数的指针（void*，这里传 cfg）
 *   优先级数字越大越优先；同核同优先级 → 时间片轮转
 *   vTaskDelayUntil 是"按固定周期"；vTaskDelay 是"从现在起等 N 个 tick"（会累积漂移）
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "L2";

/*============================================================================
 * 实验参数 —— 就改这几行，做四种观察
 *============================================================================*/
/* ① 基线：两个任务都钉在 Core 0、周期 100ms、各忙 40ms
 *    实测：总需求 80% < 100%，周期稳在 100ms；此时优先级 5 和 6 看不出差别 */
#define TASK_A_CORE        0
#define TASK_A_PRIO        5
#define TASK_A_PERIOD_MS   100
#define TASK_A_BUSY_MS     60

#define TASK_B_CORE        0
#define TASK_B_PRIO        5
#define TASK_B_PERIOD_MS   100
#define TASK_B_BUSY_MS     60

/* ② 实验二：把 TASK_B_CORE 改成 1 → 两个任务真正并行，周期恢复稳定
 * ③ 实验三：先把两个任务的 busy 都加到 70ms（总需求 140%）→ 超载；
 *    再比较 A=6/B=5 与 A=5/B=6 两种优先级分配，差异才会显形
 * ④ 实验四：把栈开到 1024（字节，即 1KB）→ 观察栈溢出（L4 预告） */
#define STACK_BYTES        4096   /* 注意：单位是字节 byte，不是原生 FreeRTOS 的字 */

/* 实验模块的接口从头文件来，不手写 extern（改原型时能立刻编译报错） */
#include "race_demo.h"
#include "priority_inversion.h"
#include "queue_demo.h"
#include "queue_advanced.h"
#include "semaphore_demo.h"
#include "event_timer_demo.h"
#include "intr_demo.h"
#include "heap_demo.h"
#include "crash_demo.h"
#include "fault_demo.h"
#include "dual_core_demo.h"
#include "net_client.h"
#include "camera_demo.h"
#include "display_demo.h"
#include "lvgl_demo.h"
#include "camera_preview.h"
#include "camera_ui.h"

/*============================================================================
 * 实验选择器 —— 改这一行就能切换要跑的实验（0~12）
 * 切换后重新 build + flash 即可；调试器里按 F5 就是调试这个实验。
 *============================================================================*/
#define EXP_L2_TASKS        0   /* L2：两个周期任务 + 运行时间统计（默认） */
#define EXP_RACE            1   /* B2-1：数据竞争（race_demo.c） */
#define EXP_PRIO_INVERSION  2   /* B2-2：优先级翻转（priority_inversion.c） */
#define EXP_QUEUE           3   /* B3：队列丢包（queue_demo.c） */
#define EXP_QUEUE_ADV       4   /* B3：队列进阶/队列集（queue_advanced.c） */
#define EXP_SEMAPHORE       5   /* B4：信号量/任务通知（semaphore_demo.c） */
#define EXP_EVENT_TIMER     6   /* B5：事件组 + 软件定时器（event_timer_demo.c） */
#define EXP_INTR            7   /* B6：中断级调用（intr_demo.c） */
#define EXP_HEAP            8   /* B7：内存管理（heap_demo.c） */
#define EXP_CRASH           9   /* D1：故意崩溃（crash_demo.c） */
#define EXP_FAULT           10  /* D3：四类故障（fault_demo.c） */
#define EXP_DUAL_CORE       11  /* 双芯第二课：双核任务与核间通信 */
#define EXP_NET_CLIENT      12  /* P6：lwIP socket 客户端 + 错误码 + 退避重连 */
#define EXP_CAMERA          13  /* P5：SC2336 + MIPI-CSI 采集 */
#define EXP_DISPLAY         14  /* P4：JD9165 1024x600 点屏（彩条） */
#define EXP_LVGL            15  /* P4：LVGL 界面 + GT911 触摸 */
#define EXP_CAM_PREVIEW     16  /* P5：SC2336 → JD9165 预览 */
#define EXP_CAM_UI          17  /* P5：LVGL 叠在相机预览上 */

#define RUN_EXPERIMENT      EXP_CAM_UI
/*============================================================================
 * 每个任务自己的配置：靠 xTaskCreate 的参数传进去，而不是全局变量
 *============================================================================*/
typedef struct {
    const char *name;      /* 日志标签 + 任务名 */
    uint32_t    period_ms; /* 周期 */
    uint32_t    busy_ms;   /* 每周期里"死占 CPU"多久 */
    uint32_t    counter;   /* 跑了几轮 */
    uint32_t    work_units;/* 每轮要做的计算量 = busy_ms × 标定值 */
} worker_cfg_t;

static worker_cfg_t s_cfg_a = { "TASK_A", TASK_A_PERIOD_MS, TASK_A_BUSY_MS, 0, 0 };
static worker_cfg_t s_cfg_b = { "TASK_B", TASK_B_PERIOD_MS, TASK_B_BUSY_MS, 0, 0 };

static TaskHandle_t s_handle_a = NULL;
static TaskHandle_t s_handle_b = NULL;

/*============================================================================
 * 工作量模型 —— 两种，用宏切换，用来对比"同一负载的两种超载表现"
 *
 * 模式 0（墙钟）：忙到墙钟过 busy_ms 毫秒为止。被抢占照样退出
 *                 → 周期准，但 IDLE 被饿死，表现为 task_wdt 报警
 * 模式 1（计算量）：做固定次数的计算，被抢占就算不完
 *                 → 周期漂移。真实工程的负载都是这一种
 *============================================================================*/
#define USE_CPU_WORK        1       /* 1 = 按计算量；0 = 按墙钟（旧行为） */

static volatile uint32_t s_cpu_sink; /* 防止编译器把纯计算优化掉 */

/* 做 iterations 次固定计算：里面没有任何时间判断，纯粹是计算量 */
static void cpu_work(uint32_t iterations)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        acc += i * 2654435761u;     /* 黄金比例常数，乘法不易被优化掉 */
        acc ^= acc >> 7;
    }
    s_cpu_sink = acc;               /* 写出去，编译器就不敢删这段循环 */
}

/* 旧行为：忙到墙钟过 ms 毫秒（被抢占也照样在 ms 墙钟后退出）
 * 只在 USE_CPU_WORK=0 时才会被调用，所以用条件编译包起来，避免 unused 警告 */
#if !USE_CPU_WORK
static void busy_work_wall_ms(uint32_t ms)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < (int64_t)ms * 1000) {
        /* 空转 */
    }
}
#endif

/*---------------------------------------------------------------------------
 * 标定：1ms 的 CPU 时间大约能做多少次 cpu_work 迭代
 * 为什么必须标定：主频、编译优化级别、缓存命中率都会影响，写死数字不可移植
 * 标定只在启动时做一次，此时系统还闲，误差可接受（这本身也是个工程取舍）
 *-------------------------------------------------------------------------*/
static uint32_t s_units_per_ms = 0;

static uint32_t calibrate_units_per_ms(void)
{
    const uint32_t probe_units = 20000;   /* 探测用量：别太大，否则启动变慢 */

    int64_t t0 = esp_timer_get_time();
    cpu_work(probe_units);
    int64_t t1 = esp_timer_get_time();

    int64_t elapsed_us = t1 - t0;
    if (elapsed_us <= 0) {
        return 1;                         /* 防御：除零保护 */
    }
    return (uint32_t)(((uint64_t)probe_units * 1000u) / (uint64_t)elapsed_us);
}

/*============================================================================
 * worker 任务：两个任务共用同一份代码，行为由 cfg 决定
 *============================================================================*/
static void worker_task(void *arg)
{
    worker_cfg_t *cfg = (worker_cfg_t *)arg;

    ESP_LOGI(cfg->name, "启动 | Core=%d 优先级=%d 栈剩余=%u 字节 每轮计算量=%u",
             xPortGetCoreID(),
             (int)uxTaskPriorityGet(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)cfg->work_units);

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
#if USE_CPU_WORK
        /* 按计算量：被抢占就算不完，这一轮就要超期 */
        cpu_work(cfg->work_units);
#else
        /* 按墙钟：被抢占也照样在 busy_ms 后退出，周期不变 */
        busy_work_wall_ms(cfg->busy_ms);
#endif
        cfg->counter++;

        /* 每 10 轮报一次，避免日志本身干扰调度 */
        if (cfg->counter % 10 == 0) {
            ESP_LOGI(cfg->name, "已运行 %u 轮", (unsigned)cfg->counter);
        }

        /* 固定周期等待：下一次唤醒时刻 = 上一次 + period，不会漂移 */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(cfg->period_ms));
    }
}

/*============================================================================
 * monitor 任务：每 5 秒打印一次 FreeRTOS 运行时间统计
 * 这是排查"谁在偷占 CPU"的第一把工具，比 printf 管用
 *============================================================================*/
static void monitor_task(void *arg)
{
    static char stats_buf[1024];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        memset(stats_buf, 0, sizeof(stats_buf));
        vTaskGetRunTimeStats(stats_buf);

        ESP_LOGI("MON", "==== 各任务 CPU 占比 ====");
        printf("%s\n", stats_buf);

        ESP_LOGI("MON", "A 跑了 %u 轮，栈剩余 %u 字节 | B 跑了 %u 轮，栈剩余 %u 字节",
                 (unsigned)s_cfg_a.counter, (unsigned)uxTaskGetStackHighWaterMark(s_handle_a),
                 (unsigned)s_cfg_b.counter, (unsigned)uxTaskGetStackHighWaterMark(s_handle_b));
    }
}

/*============================================================================
 * 入口：app_main 本身就是一个任务（main 任务），建完子任务就退出
 *============================================================================*/
void app_main(void)
{
    /* ---- 按 RUN_EXPERIMENT 分发到对应实验；EXP_L2_TASKS 会落到下面的原代码 ---- */
    switch (RUN_EXPERIMENT) {
    case EXP_RACE:           race_demo_start();          return;
    case EXP_PRIO_INVERSION: priority_inversion_start(); return;
    case EXP_QUEUE:          queue_demo_start();         return;
    case EXP_QUEUE_ADV:      queue_advanced_start();     return;
    case EXP_SEMAPHORE:      semaphore_demo_start();     return;
    case EXP_EVENT_TIMER:    event_timer_demo_start();   return;
    case EXP_INTR:           intr_demo_start();          return;
    case EXP_HEAP:           heap_demo_start();          return;
    case EXP_CRASH:          crash_demo_start();         return;
    case EXP_FAULT:          fault_demo_start();         return;
    case EXP_DUAL_CORE:      dual_core_demo_start();     return;
    case EXP_NET_CLIENT:     net_client_start(NET_CLIENT_DEFAULT_HOST,
                                              NET_CLIENT_DEFAULT_PORT); return;
    case EXP_CAMERA:         camera_demo_start();         return;
    case EXP_DISPLAY:        display_demo_start();        return;
    case EXP_LVGL:           lvgl_demo_start();           return;
    case EXP_CAM_PREVIEW:    camera_preview_start();      return;
    case EXP_CAM_UI:         camera_ui_start();           return;
    case EXP_L2_TASKS:
    default:                 break;   /* 继续执行下面的 L2 代码 */
    }

    ESP_LOGI(TAG, "FreeRTOS 实验开始，芯片核心数 = %d", (int)portNUM_PROCESSORS);

    /* ---- 先标定：1ms CPU 时间 ≈ 多少次计算迭代 ---- */
    s_units_per_ms = calibrate_units_per_ms();
    ESP_LOGI(TAG, "标定：1ms CPU ≈ %u 次迭代", (unsigned)s_units_per_ms);

    /* 把"我想要多久的 CPU 时间"换算成"做多少次计算" */
    s_cfg_a.work_units = (uint32_t)((uint64_t)s_units_per_ms * s_cfg_a.busy_ms);
    s_cfg_b.work_units = (uint32_t)((uint64_t)s_units_per_ms * s_cfg_b.busy_ms);
    ESP_LOGI(TAG, "每轮计算量：A=%u，B=%u 次迭代",
             (unsigned)s_cfg_a.work_units, (unsigned)s_cfg_b.work_units);

    BaseType_t ret;

    ret = xTaskCreatePinnedToCore(worker_task, s_cfg_a.name,
                                  STACK_BYTES, &s_cfg_a,
                                  TASK_A_PRIO, &s_handle_a, TASK_A_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 TASK_A 失败");
        abort();
    }

    ret = xTaskCreatePinnedToCore(worker_task, s_cfg_b.name,
                                  STACK_BYTES, &s_cfg_b,
                                  TASK_B_PRIO, &s_handle_b, TASK_B_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 TASK_B 失败");
        abort();
    }

    /* monitor 放 Core 1、优先级 3，保证它不跟 worker 抢 */
    ret = xTaskCreatePinnedToCore(monitor_task, "MON",
                                  4096, NULL,
                                  3, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 MON 失败");
        abort();
    }

    ESP_LOGI(TAG, "三个任务已创建，主任务退出（vTaskDelete(NULL)）");

    /* main 任务不参与业务，直接删掉自己，省 4KB 栈 */
    vTaskDelete(NULL);
}
