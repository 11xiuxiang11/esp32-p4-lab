/*
 * crash_demo.h —— 故意崩溃，用于练习 GDB 调试（D1）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动后 3 秒故意触发一次空指针写入（Load/Store access fault）
 *
 * 前提：sdkconfig 里 CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y
 * 效果：CPU 停在崩溃点，等 GDB 通过串口连上来（不会自动重启）
 */
void crash_demo_start(void);

#ifdef __cplusplus
}
#endif
