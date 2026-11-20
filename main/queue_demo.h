/*
 * queue_demo.h —— 队列实验（B3）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动生产者/消费者队列实验，跑 5 秒后打印统计
 *
 * 用 queue_demo.c 顶部的宏切换实验组：
 *   CONSUMER_PERIOD_MS : 消费者周期
 *     50ms（消费 20/s，慢于生产的 100/s）→ 队列很快满，大量丢包
 *      5ms（消费 200/s，快于生产）        → 一个都不丢
 */
void queue_demo_start(void);

#ifdef __cplusplus
}
#endif
