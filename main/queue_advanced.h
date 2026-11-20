/*
 * queue_advanced.h —— 队列进阶实验（B3 续）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动队列进阶实验，跑 5 秒后打印统计
 *
 * 用 queue_advanced.c 顶部的 QADV_MODE 选模式：
 *   0 = 丢弃（xQueueSend 超时 0，满了就丢新数据）
 *   1 = 覆盖（队列长度 1 + xQueueOverwrite，永远只留最新值）
 *   2 = 背压（xQueueSend 永久等待，生产被消费者拖慢，一个不丢）
 *   3 = 队列集（两个生产者 + 一个消费者，用 xQueueSelectFromSet 同时等两个队列）
 */
void queue_advanced_start(void);

#ifdef __cplusplus
}
#endif
