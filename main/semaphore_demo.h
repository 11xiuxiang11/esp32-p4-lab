/*
 * semaphore_demo.h —— 信号量实验（B4）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动信号量实验
 *
 * 用 semaphore_demo.c 顶部的 SEM_MODE 切换：
 *   0 = 二值信号量做事件通知（测唤醒延迟）
 *   1 = 计数信号量做资源池（验证最大并发 = 初始计数）
 */
void semaphore_demo_start(void);

#ifdef __cplusplus
}
#endif
