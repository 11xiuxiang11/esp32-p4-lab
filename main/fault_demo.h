/*
 * fault_demo.h —— 四类典型故障（D3）的对外接口
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动故障实验
 *
 * 用 fault_demo.c 顶部的 FAULT_MODE 切换：
 *   0 = 栈溢出（任务栈只有 1KB，却递归 40 层）
 *   1 = 堆耗尽（一直 malloc 到失败，再看创建任务会不会失败）
 *   2 = 看门狗（两个任务同核占满 CPU，IDLE 永远轮不上）
 *   3 = 死锁（两个任务交叉拿两把互斥量）
 *
 * 说明：故障 0 会触发 panic（默认停在 GDB Stub，不自动重启）；
 *       故障 2 会触发 task_wdt；故障 3 不会报错，只会"系统静止"。
 */
void fault_demo_start(void);

#ifdef __cplusplus
}
#endif
