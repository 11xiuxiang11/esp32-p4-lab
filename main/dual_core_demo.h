/*
 * dual_core_demo.h —— P4 双核任务分工与核间通信（双芯第二课）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动双核实验
 *
 * 两个任务做 ping-pong：A 发令牌 → B 收到后立刻回发 → A 收到算一次往返，
 * 跑 N 次取平均，单位纳秒。
 *
 * 用 dual_core_demo.c 顶部的 DC_MODE 切换：
 *   DC_MODE_SAME_CORE  = 两个任务都绑 core0（同核）
 *   DC_MODE_CROSS_CORE = A 绑 core0，B 绑 core1（跨核）
 *
 * 对比两种模式的单次往返耗时，就能看出"跨核通信"的代价。
 */
void dual_core_demo_start(void);

#ifdef __cplusplus
}
#endif
