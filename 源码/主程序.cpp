// 操作系统核心模拟器 — 主程序入口
// 北京林业大学 信息学院 操作系统A课程设计

#include "系统模拟器.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

int main() {
    // Windows 控制台切到 UTF-8，不然中文乱码
#ifdef _WIN32
    system("chcp 65001 > nul 2>&1");
    SetConsoleOutputCP(CP_UTF8);
#endif

    OSSimulator simulator;
    simulator.init();   // 初始化：加载存档 or 创建 init 进程
    simulator.run();    // 主循环：读命令→执行→输出

    return 0;
}
