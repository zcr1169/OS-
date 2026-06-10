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

// 操作系统模拟器主控类
// 后端实例(获文件锁): 后台线程维护状态, 定期保存到物理文件, 监听观察者命令文件
// 观察者实例: 守护线程定时轮询 os_state.bin 时间戳, 变化时自动重载; 命令写入 commands.txt 由后端执行
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

    // 观察者守护线程: 轮询 os_state.bin 时间戳
    void pollLoop();
    // 后端检查观察者发来的命令文件
    void checkObserverCommands();

    std::string handleUserCmd(const CommandParser::Command& cmd);
    std::string handleProcessCmd(const CommandParser::Command& cmd);
    std::string handleSchedulerCmd(const CommandParser::Command& cmd);
    std::string handleMemoryCmd(const CommandParser::Command& cmd);
    std::string handlePersistenceCmd(const CommandParser::Command& cmd);
    std::string handleOverviewCmd();

    ProcessManager processMgr_;
    MemoryManager memoryMgr_;
    UserManager userMgr_;
    Scheduler scheduler_;

    MessageQueue<CommandParser::Command> cmdQueue_;
    MessageQueue<std::string> resultQueue_;

    FileLock backendLock_;
    bool isBackend_;

    // 观察者轮询
    std::thread pollThread_;
    int64_t lastPollTime_;
    bool localLoggedIn_ = false;
    std::string localUser_;

    std::string dataDir_;  // 数据文件夹路径(基于exe所在目录)
    std::atomic<bool> running_;
    std::thread backendThread_;

    // 换出内存记录(供swap_in恢复): PID → 内存块列表
    std::unordered_map<int32_t, std::vector<std::pair<int32_t,int32_t>>> swappedOut_;
};
