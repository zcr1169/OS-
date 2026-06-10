// ============================================================
// 操作系统模拟器 — 主控类头文件
//
// 这是整个程序的"总控"，包含所有子模块的实例，
// 管理线程生命周期和命令分发。
// ============================================================
#pragma once
#include "进程管理器.h"
#include "内存管理器.h"
#include "用户管理器.h"
#include "调度器.h"
#include "状态持久化.h"
#include "命令解析器.h"
#include "消息队列.h"
#include "文件锁.h"
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <utility>

/**
 * OSSimulator — 操作系统模拟器主控类
 *
 * 职责：
 *   1. 持有所有子模块实例（进程管理器、内存管理器等）
 *   2. 管理三个线程（前台、后台、调度器）
 *   3. 多实例角色判断（后端 vs 观察者）
 *   4. 命令解析和分发
 *
 * 多实例工作模式：
 *   后端实例（获得文件锁）：
 *     - 拥有后台线程，直接执行命令
 *     - 每命令后 save 持久化
 *     - 监听 commands.txt 处理观察者请求
 *
 *   观察者实例（未获文件锁）：
 *     - 没有后台线程，命令写入 commands.txt
 *     - pollLoop 线程轮询 os_state.bin 时间戳
 *     - 可自动升级为后端
 */
class OSSimulator {
public:
    OSSimulator();
    ~OSSimulator();

    void init();
    void run();
    std::string executeCommand(const CommandParser::Command& cmd);
    bool isBackend() const { return isBackend_; }

    std::string stateFilePath() const { return dataDir_ + "/os_state.bin"; }
    std::string lockFilePath() const { return dataDir_ + "/os_instance.lock"; }
    std::string cmdFilePath() const { return dataDir_ + "/commands.txt"; }

private:
    void backendLoop();
    void showWelcome();
    void showHelp();
    void pollLoop();
    void checkObserverCommands();

    std::string handleUserCmd(const CommandParser::Command& cmd);
    std::string handleProcessCmd(const CommandParser::Command& cmd);
    std::string handleSchedulerCmd(const CommandParser::Command& cmd);
    std::string handleMemoryCmd(const CommandParser::Command& cmd);
    std::string handlePersistenceCmd(const CommandParser::Command& cmd);
    std::string handleOverviewCmd();

    // 核心子模块
    ProcessManager processMgr_;
    MemoryManager memoryMgr_;
    UserManager userMgr_;
    Scheduler scheduler_;

    // 线程间通信
    MessageQueue<CommandParser::Command> cmdQueue_;   // 前台→后台的命令队列
    MessageQueue<std::string> resultQueue_;            // 后台→前台的结果队列

    // 多实例控制
    FileLock backendLock_;
    bool isBackend_;

    // 观察者相关
    std::thread pollThread_;
    int64_t lastPollTime_;
    bool localLoggedIn_ = false;    // 观察者本地记录的登录状态（用于提示符）
    std::string localUser_;

    std::string dataDir_;                // 数据文件夹路径（基于 exe 目录）
    std::atomic<bool> running_;
    std::thread backendThread_;

    // 换出内存记录（swap_out 保存，swap_in 恢复）
    std::unordered_map<int32_t, std::vector<std::pair<int32_t,int32_t>>> swappedOut_;
};
