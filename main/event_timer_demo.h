/*
 * event_timer_demo.h —— 事件组 + 软件定时器实验（B5）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动事件组 / 软件定时器实验
 *
 * 用 event_timer_demo.c 顶部的 ET_MODE 切换：
 *   0 = 事件组门控：三个初始化任务各自置位，采集任务等三个条件全满足
 *   1 = 软件定时器：周期定时器测抖动 + 单次定时器做"长按 2 秒"（靠 Reset 续命）
 */
void event_timer_demo_start(void);

#ifdef __cplusplus
}
#endif
