// 操作系统模拟器 — 主控类
// 协调所有子系统，管理三线程，处理多实例（后端/观察者）
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
    std::string handleLogCmd(const CommandParser::Command& cmd);

    // 线程日志打印
    void logPrint(const char* role, const std::string& msg);
    std::atomic<bool> logEnabled_{false};

    ProcessManager processMgr_;
    MemoryManager memoryMgr_;
    UserManager userMgr_;
    Scheduler scheduler_;

    MessageQueue<CommandParser::Command> cmdQueue_;
    MessageQueue<std::string> resultQueue_;

    FileLock backendLock_;
    bool isBackend_;

    std::thread pollThread_;
    int64_t lastPollTime_;
    bool localLoggedIn_ = false;
    std::string localUser_;

    std::string dataDir_;
    std::atomic<bool> running_;
    std::thread backendThread_;

    std::unordered_map<int32_t, std::vector<std::pair<int32_t,int32_t>>> swappedOut_;
};
