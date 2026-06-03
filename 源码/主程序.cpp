// 操作系统核心模拟器 - 主程序入口
// 北京林业大学 信息学院 操作系统A课程设计
// 功能: 进程管理 + 多级反馈队列调度 + 内存分配 + 状态持久化

#include "系统模拟器.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

int main() {
    // 设置控制台编码为UTF-8 (Windows)
#ifdef _WIN32
    system("chcp 65001 > nul 2>&1");
    SetConsoleOutputCP(CP_UTF8);
#endif

    OSSimulator simulator;

    // 初始化系统
    simulator.init();

    // 进入主循环
    simulator.run();

    return 0;
}
