/*
 * priority_inversion.h —— 优先级翻转实验（B2-2）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动三任务（L/M/H）优先级翻转实验，约 2 秒后结束
 *
 * 用 priority_inversion.c 顶部的两个宏切换实验组：
 *   INCLUDE_MID_TASK : 是否创建中优先级任务 M
 *   USE_MUTEX_LOCK   : 互斥量（有继承）/ 二值信号量（无继承）
 */
void priority_inversion_start(void);

#ifdef __cplusplus
}
#endif
