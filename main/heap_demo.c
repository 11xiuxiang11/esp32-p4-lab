/*
 * heap_demo.c —— 内存管理实验（B7，对应视频 18）
 *
 * 三个模式，改顶部 HEAP_MODE 切换：
 *
 *   0 = 制造碎片
 *       先分配 8 块 2KB，再分配 8 块 128B，然后释放全部 2KB 块
 *       → 内存里出现 8 个 2KB 的"洞"，被 128B 的小块隔开
 *       → 此时【总空闲很多】，但【最大连续块只有 ~2KB】
 *       → 申请 8KB 会失败：这就是"碎片"，不是"内存不够"
 *       最后释放所有小块，碎片合并，再申请 8KB 就成功
 *
 *   1 = 测任务栈水位
 *       三档栈（2048 / 4096 / 8192 字节），各自调用同样的深调用函数
 *       用 uxTaskGetStackHighWaterMark 看"历史最低剩余"
 *       → 结论：栈大小应该按实测峰值 × 1.5~2 倍来给，不是拍脑袋
 *
 *   2 = 探测内存区域
 *       分别向 内部 RAM / DMA 可用内存 / PSRAM 申请
 *       本工程没开 CONFIG_SPIRAM，所以 PSRAM 那份会失败 —— 这本身是知识点
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "heap_demo.h"

static const char *TAG = "HEAP";

/*============================================================================
 * ① 模式选择
 *============================================================================*/
#define HEAP_MODE_FRAG    0
#define HEAP_MODE_STACK   1
#define HEAP_MODE_CAPS    2

#define HEAP_MODE         HEAP_MODE_CAPS   /* ← 改这一行切换实验 */

/*============================================================================
 * ② 公共参数
 *============================================================================*/
#define INTERNAL_CAPS     (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define TASK_STACK_BYTES  4096u
#define MAIN_PRIO_BOOST   7

/* 打印当前内存状况：总空闲 + 最大连续块（后者才是能不能分配成功的关键）
 * 模式 2 有自己的打印方式，用不到这个函数，所以条件编译掉，避免 unused 警告 */
#if HEAP_MODE != HEAP_MODE_CAPS
static void print_mem(const char *stage)
{
    size_t free_now    = heap_caps_get_free_size(INTERNAL_CAPS);
    size_t largest_now = heap_caps_get_largest_free_block(INTERNAL_CAPS);
    size_t min_ever    = heap_caps_get_minimum_free_size(INTERNAL_CAPS);

    ESP_LOGI(TAG, "[%s] 总空闲 %u B | 最大连续块 %u B | 历史最低 %u B",
             stage, (unsigned)free_now, (unsigned)largest_now, (unsigned)min_ever);
}
#endif /* HEAP_MODE != HEAP_MODE_CAPS */

/*============================================================================
 * ③ 模式 0：制造碎片
 *============================================================================*/
#if HEAP_MODE == HEAP_MODE_FRAG

#define BIG_SIZE        2048
#define BIG_COUNT       8
#define SMALL_SIZE      128
#define SMALL_COUNT     8
#define WANT_SIZE       8192        /* 最后要申请的连续块大小 */

static void *s_big[BIG_COUNT];
static void *s_small[SMALL_COUNT];

void heap_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 0：制造碎片 ====");
    print_mem("初始");

    /* 1. 先分配 8 块 2KB */
    for (int i = 0; i < BIG_COUNT; i++) {
        s_big[i] = heap_caps_malloc(BIG_SIZE, INTERNAL_CAPS);
        if (s_big[i] == NULL) {
            ESP_LOGE(TAG, "第 %d 块 2KB 分配失败（初始内存就不够）", i);
            return;
        }
    }
    print_mem("分配 8×2KB 之后");

    /* 2. 再分配 8 块 128B —— 它们会把 2KB 块之间的空隙"钉住" */
    for (int i = 0; i < SMALL_COUNT; i++) {
        s_small[i] = heap_caps_malloc(SMALL_SIZE, INTERNAL_CAPS);
    }
    print_mem("再分配 8×128B 之后");

    /* 3. 释放全部 2KB 块 → 出现 8 个洞，但被小块隔开，无法合并 */
    for (int i = 0; i < BIG_COUNT; i++) {
        heap_caps_free(s_big[i]);
        s_big[i] = NULL;
    }
    print_mem("释放全部 2KB 之后（碎片的现场）");

    /* 4. 此时总空闲远大于 8KB，但最大连续块只有 ~2KB */
    void *want = heap_caps_malloc(WANT_SIZE, INTERNAL_CAPS);
    if (want == NULL) {
        ESP_LOGW(TAG, "申请 %d B 连续块失败 —— 总空闲够，但没有连续空间（这就是碎片）",
                 WANT_SIZE);
    } else {
        ESP_LOGW(TAG, "申请 %d B 成功了（说明没碎成预期那样，可以多跑几次）", WANT_SIZE);
        heap_caps_free(want);
    }

    /* 5. 释放小块 → 洞连起来 → 再申请同样的块就成功 */
    for (int i = 0; i < SMALL_COUNT; i++) {
        if (s_small[i] != NULL) {
            heap_caps_free(s_small[i]);
            s_small[i] = NULL;
        }
    }
    print_mem("释放全部小块之后（碎片合并）");

    want = heap_caps_malloc(WANT_SIZE, INTERNAL_CAPS);
    if (want != NULL) {
        ESP_LOGI(TAG, "合并后再申请 %d B 成功 —— 证明刚才失败是【碎片】而不是【不够】",
                 WANT_SIZE);
        heap_caps_free(want);
    } else {
        ESP_LOGE(TAG, "合并后仍然失败，说明这块内存确实不够");
    }

    print_mem("结束");
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* HEAP_MODE_FRAG */

/*============================================================================
 * ④ 模式 1：测任务栈水位
 *============================================================================*/
#if HEAP_MODE == HEAP_MODE_STACK

/* 带 64 字节局部数组的递归，模拟"调用链比较深"的函数 */
static uint32_t deep_call(uint32_t depth)
{
    volatile uint8_t buf[64];
    buf[0] = (uint8_t)depth;
    buf[63] = (uint8_t)(depth + 1);

    if (depth == 0) {
        return (uint32_t)buf[0] + buf[63];
    }
    return deep_call(depth - 1) + (uint32_t)buf[0];
}

typedef struct {
    const char *name;
    uint32_t    stack_bytes;
    uint32_t    depth;
} stack_cfg_t;

static stack_cfg_t s_cfg[] = {
    { "S2K", 2048u, 4 },
    { "S4K", 4096u, 4 },
    { "S8K", 8192u, 4 },
};
#define STACK_TASK_NUM  (sizeof(s_cfg) / sizeof(s_cfg[0]))

static void stack_probe_task(void *arg)
{
    stack_cfg_t *cfg = (stack_cfg_t *)arg;

    volatile uint32_t sink = deep_call(cfg->depth);

    /* 栈单位是字节；返回值也是字节（ESP-IDF 的约定） */
    UBaseType_t remain = uxTaskGetStackHighWaterMark(NULL);
    uint32_t used = (remain <= cfg->stack_bytes)
                    ? (cfg->stack_bytes - (uint32_t)remain) : 0;

    ESP_LOGI(TAG, "%s：栈 %u B，峰值用掉约 %u B，剩余 %u B（结论：%u B 够用，但有 %u 的余量）",
             cfg->name, (unsigned)cfg->stack_bytes, (unsigned)used,
             (unsigned)remain, (unsigned)cfg->stack_bytes, (unsigned)remain);

    (void)sink;
    vTaskDelete(NULL);
}

void heap_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 1：测任务栈水位（深度 %u 的递归）====", s_cfg[0].depth);
    print_mem("开始前");

    UBaseType_t saved_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, MAIN_PRIO_BOOST);

    for (size_t i = 0; i < STACK_TASK_NUM; i++) {
        BaseType_t ret = xTaskCreatePinnedToCore(stack_probe_task, s_cfg[i].name,
                                                 s_cfg[i].stack_bytes, &s_cfg[i],
                                                 5, NULL, 0);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "创建 %s 失败（栈要得太大？）", s_cfg[i].name);
        }
    }

    vTaskPrioritySet(NULL, saved_prio);

    vTaskDelay(pdMS_TO_TICKS(1000));

    print_mem("结束后");
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* HEAP_MODE_STACK */

/*============================================================================
 * ⑤ 模式 2：探测内存区域（内部 RAM / DMA / PSRAM）
 *============================================================================*/
#if HEAP_MODE == HEAP_MODE_CAPS

typedef struct {
    const char *name;
    uint32_t    caps;
} caps_item_t;

static const caps_item_t s_caps_list[] = {
    { "内部 RAM (8bit)",  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT },
    { "DMA 可用",         MALLOC_CAP_DMA },
    { "PSRAM",            MALLOC_CAP_SPIRAM },
};

void heap_demo_start(void)
{
    ESP_LOGI(TAG, "==== 模式 2：探测内存区域 ====");

    for (size_t i = 0; i < sizeof(s_caps_list) / sizeof(s_caps_list[0]); i++) {
        const caps_item_t *item = &s_caps_list[i];

        size_t free_size = heap_caps_get_free_size(item->caps);
        size_t largest   = heap_caps_get_largest_free_block(item->caps);

        void *p = heap_caps_malloc(1024, item->caps);
        ESP_LOGI(TAG, "%-16s 空闲 %u B | 最大连续块 %u B | 申请 1KB %s",
                 item->name, (unsigned)free_size, (unsigned)largest,
                 p ? "成功" : "失败");

        if (p != NULL) {
            heap_caps_free(p);
        }
    }

    ESP_LOGI(TAG, "说明：PSRAM 失败是因为本工程没开 CONFIG_SPIRAM（menuconfig 里打开才有）");
    ESP_LOGI(TAG, "==== 实验结束 ====");
}

#endif /* HEAP_MODE_CAPS */
