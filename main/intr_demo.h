/*
 * intr_demo.h —— 中断级调用实验（B6）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动中断实验：硬件定时器每 1ms 触发一次中断，ISR 只发任务通知，
 *        任务统计"从 ISR 到任务被唤醒"的延迟。
 *
 * 预期：
 *   - ISR 每秒被调用 1000 次（1ms 周期）
 *   - 任务每次被唤醒的延迟通常在几十微秒量级
 *   - 若把任务优先级降得很低、或加一个干扰任务，延迟会变大
 */
void intr_demo_start(void);

#ifdef __cplusplus
}
#endif
