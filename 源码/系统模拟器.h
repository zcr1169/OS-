#pragma once
#include "进程管理器.h"
#include "内存管理器.h"
#include "用户管理器.h"
#include "调度器.h"
#include "状态持久化.h"
#include "命令解析器.h"
#include "消息队列.h"
#include "文件锁.h"
#include "IPC管道.h"
#include <string>
#include <thread>
#include <atomic>
#include <iostream>

// 操作系统模拟器主控类
// 协调所有子系统, 管理前后台线程
// 后端实例(获文件锁): 运行后台 + 管道服务, 集中处理命令
// 观察者实例: 通过管道转发命令到后端
class OSSimulator {
public:
    OSSimulator();
    ~OSSimulator();

    // 初始化系统(创建init进程等)
    void init();

    // 主循环(前台线程入口)
    void run();

    // 处理单条命令(后台/管道线程调用)
    std::string executeCommand(const CommandParser::Command& cmd);

    // 本实例是否为后端
    bool isBackend() const { return isBackend_; }

    // 状态文件路径
    std::string stateFilePath() const {
        return "./数据/os_state.bin";
    }

    // 锁文件路径(多实例互斥用)
    std::string lockFilePath() const {
        return "./数据/os_instance.lock";
    }

private:
    // 后台线程主循环
    void backendLoop();

    // 显示欢迎信息
    void showWelcome();

    // 显示命令帮助
    void showHelp();

    // 各分类命令处理
    std::string handleUserCmd(const CommandParser::Command& cmd);
    std::string handleProcessCmd(const CommandParser::Command& cmd);
    std::string handleSchedulerCmd(const CommandParser::Command& cmd);
    std::string handleMemoryCmd(const CommandParser::Command& cmd);
    std::string handlePersistenceCmd(const CommandParser::Command& cmd);
    std::string handleOverviewCmd();

    // === 核心模块 ===
    ProcessManager processMgr_;
    MemoryManager memoryMgr_;
    UserManager userMgr_;
    Scheduler scheduler_;

    // === 线程间通信 ===
    MessageQueue<CommandParser::Command> cmdQueue_;
    MessageQueue<std::string> resultQueue_;

    // === 多实例控制 ===
    FileLock backendLock_;
    bool isBackend_;  // 本实例是否为活动后端

    // === 命名管道IPC ===
    PipeServer pipeServer_;
    PipeClient pipeClient_;

    // === 观察者本地登录跟踪(用于提示符显示) ===
    bool localLoggedIn_ = false;
    std::string localUser_;

    // === 线程控制 ===
    std::atomic<bool> running_;
    std::thread backendThread_;
};
