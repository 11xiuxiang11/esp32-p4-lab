/*
 * race_demo.h —— 数据竞争演示（B2-1）的对外接口
 *
 * 头文件只放【声明】：函数原型、类型、宏、extern 变量。
 * 不放函数体、不放变量定义（否则多个 .c 一 include 就重复定义）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动数据竞争对照实验（紧循环 vs 宽窗口），约 1 秒后打印两轮结果
 */
void race_demo_start(void);

#ifdef __cplusplus
}
#endif
