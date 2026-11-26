// log.h - 极简文本日志，用于排查「开机自启到底有没有跑起来」
//
// 日志写在 exe 同目录的 FanCtrl.log，只记关键节点（启动参数、初始化结果、退出原因、
// 计划任务操作），不记每次采样，避免长期运行把磁盘写满。
#pragma once

#include <windows.h>

void LogInit();                                // 确定日志路径（exe 同目录），顺带做一次尺寸检查
void Logf(const wchar_t* fmt, ...);             // 追加一行，带毫秒级时间戳

// 进程是否以管理员权限运行（判断自启任务有没有真正拿到高权限）
bool IsProcessElevated();
