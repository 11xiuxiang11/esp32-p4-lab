/*
 * heap_demo.h —— 内存管理实验（B7）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动内存实验
 *
 * 用 heap_demo.c 顶部的 HEAP_MODE 切换：
 *   0 = 制造碎片：把"总空闲够、但没有连续块"的场面演出来
 *   1 = 测任务栈水位：三档栈大小，看实际用掉多少
 *   2 = 探测内存区域：内部 RAM / DMA / PSRAM（本工程没开 PSRAM，会失败）
 */
void heap_demo_start(void);

#ifdef __cplusplus
}
#endif
