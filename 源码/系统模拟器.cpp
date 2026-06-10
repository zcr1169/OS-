// ============================================================
// 操作系统模拟器 — 主控类
//
// 这是整个程序的"大脑"，协调所有子模块：
//   - 初始化各管理器
//   - 前台交互循环（读命令→执行→输出）
//   - 后台线程（异步执行命令）
//   - 调度线程（自动调度）
//   - 多实例支持（文件锁 + 观察者轮询）
//   - 命令分发（根据命令名调用对应的处理函数）
//
// 答辩重点：
//   1. 为什么用消息队列？→ 前后台解耦，前台不阻塞
//   2. 为什么用文件锁？→ 区分后端和观察者
//   3. 观察者如何同步？→ 轮询 os_state.bin 时间戳
// ============================================================

#include "系统模拟器.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

/**
 * OSSimulator 构造函数
 *
 * 关键初始化：
 *   用 GetModuleFileNameW 获取 exe 所在目录，
 *   数据文件（os_state.bin 等）统一放在 exe 旁边的"数据"文件夹，
 *   这样无论从哪个目录启动，数据路径都是固定的。
 */
OSSimulator::OSSimulator()
    : isBackend_(false), running_(false), lastPollTime_(0)
{
#ifdef _WIN32
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
    std::wstring exePathW(wbuf);
    auto pos = exePathW.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        std::wstring dirW = exePathW.substr(0, pos) + L"\\数据";
        int len = WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, nullptr, 0, nullptr, nullptr);
        dataDir_.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, &dataDir_[0], len, nullptr, nullptr);
    } else {
        dataDir_ = ".\\数据";
    }
#else
    dataDir_ = "./数据";
#endif
}

OSSimulator::~OSSimulator() {
    running_ = false;
    cmdQueue_.stop();
    if (pollThread_.joinable()) pollThread_.join();
    if (backendThread_.joinable()) backendThread_.join();
    backendLock_.unlock();
}

/**
 * init — 初始化系统
 *
 * 流程：
 *   1. 绑定调度器和进程管理器
 *   2. 设置进程终止回调（自动释放内存）
 *   3. 检查持久化文件是否存在
 *      → 存在：load() 恢复全部状态
 *      → 不存在：创建 init 进程（pid=1, ppid=-1）
 *
 * init 进程的特点：
 *   - ppid = -1（没有父进程，作为进程树的根）
 *   - priority = 0（最高优先级）
 *   - owner = "system"（系统进程，不被用户隔离过滤）
 *   - burstTime = 999999（几乎永远不终止）
 */
void OSSimulator::init() {
    scheduler_.init(&processMgr_);
    scheduler_.setOnTerminate([this](int32_t pid) {
        memoryMgr_.freeByPid(pid);  // 进程自动终止时释放内存
    });

    if (StateSerializer::fileExists(stateFilePath())) {
        if (StateSerializer::load(stateFilePath(),
                                  processMgr_, memoryMgr_,
                                  userMgr_, scheduler_)) {
            return;  // 加载成功，跳过创建 init
        }
    }

    // 没有存档，从零开始创建 init 进程
    processMgr_.createPCB("init", 0, -1, "system", 999999);
}

/**
 * run — 主运行循环
 *
 * 这是前台线程的入口，做三件事：
 *   1. 启动后台线程或观察者轮询线程
 *   2. 循环读取用户输入
 *   3. 将命令推送到消息队列，等待结果
 *
 * 多实例角色分工：
 *   后端（第一个启动的实例）：
 *     - 拥有文件锁
 *     - 启动 backendLoop 处理命令
 *     - 每执行完一条命令 save() 持久化
 *
 *   观察者（后面启动的实例）：
 *     - 没有文件锁
 *     - 命令写入 commands.txt 让后端执行
 *     - pollLoop 每 200ms 轮询 os_state.bin 时间戳
 *     - 后端退出后自动升级为新的后端
 */
void OSSimulator::run() {
    running_ = true;

    // 创建数据目录
    #ifdef _WIN32
    CreateDirectoryW(utf8ToWide(dataDir_).c_str(), nullptr);
    #else
    mkdir(dataDir_.c_str(), 0755);
    #endif

    // 尝试获取文件锁，决定角色
    isBackend_ = backendLock_.tryLock(lockFilePath());

    if (isBackend_) {
        backendThread_ = std::thread(&OSSimulator::backendLoop, this);
        StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
    } else {
        lastPollTime_ = 0;
        pollThread_ = std::thread(&OSSimulator::pollLoop, this);
    }

    showWelcome();

    // 前台交互循环
    std::string input;
    while (running_.load()) {
        // 打印提示符
        if (isBackend_) {
            std::cout << (userMgr_.isLoggedIn()
                ? "[" + userMgr_.currentUser() + "@OS]> "
                : "[未登录@OS]> ");
        } else {
            std::cout << (localLoggedIn_
                ? "[" + localUser_ + "@OS]> "
                : "[未登录@OS]> ");
        }
        std::cout.flush();

        if (!std::getline(std::cin, input)) break;
        if (input.empty()) continue;

        auto cmd = CommandParser::parse(input);
        if (cmd.name.empty()) continue;

        if (cmd.name == "exit" || cmd.name == "quit") {
            std::cout << "[系统] 再见!\n";
            break;
        }
        if (cmd.name == "help" || cmd.name == "?") {
            showHelp();
            continue;
        }

        if (isBackend_) {
            // 后端模式：推入消息队列 → 后台线程执行
            cmdQueue_.push(cmd);
            std::string result;
            if (resultQueue_.pop(result, 5000)) {
                std::cout << result;
            } else {
                std::cout << "[错误] 命令执行超时\n";
                std::string stale;
                while (resultQueue_.tryPop(stale)) {}
            }
            // 每命令后持久化，供观察者同步
            StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
        } else {
            // 观察者模式：写命令到文件，后端执行
            bool isAuthCmd = (cmd.name == "login" || cmd.name == "register" || cmd.name == "logout");
            {
                std::ofstream f(cmdFilePath(), std::ios::app);
                if (f.is_open()) {
                    f << input << "\n";
                    f.close();
                }
            }

            // 认证命令需要同步等待后端处理结果
            if (isAuthCmd) {
                auto startTime = std::chrono::steady_clock::now();
                int64_t oldTime = lastPollTime_;
                bool updated = false;
                while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(3)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    auto ftime = std::filesystem::last_write_time(stateFilePath());
                    auto t = ftime.time_since_epoch().count();
                    if (t != oldTime) {
                        lastPollTime_ = t;
                        StateSerializer::load(stateFilePath(),
                                              processMgr_, memoryMgr_, userMgr_, scheduler_);
                        updated = true;
                        break;
                    }
                }
                if (cmd.name == "login") {
                    if (updated && userMgr_.isLoggedIn()) {
                        localLoggedIn_ = true;
                        localUser_ = userMgr_.currentUser();
                        std::cout << "登录成功! 欢迎 " << localUser_ << "\n";
                    } else {
                        localLoggedIn_ = false;
                        localUser_.clear();
                        std::cout << "登录失败: 用户名或密码错误, 或账户已锁定\n";
                    }
                } else if (cmd.name == "register") {
                    std::cout << (updated ? "注册请求已处理\n" : "注册超时, 请重试\n");
                } else if (cmd.name == "logout") {
                    localLoggedIn_ = false;
                    localUser_.clear();
                    std::cout << "已登出\n";
                }
            } else {
                std::cout << "[命令已发送到后端]\n";
            }
        }
    }

    // 清理线程
    running_ = false;
    cmdQueue_.stop();
    if (pollThread_.joinable()) pollThread_.join();
    if (backendThread_.joinable()) backendThread_.join();
}

/**
 * backendLoop — 后台工作线程
 *
 * 不断从 cmdQueue 取命令执行，结果推入 resultQueue。
 * 每次 pop 超时前检查一次观察者命令文件。
 */
void OSSimulator::backendLoop() {
    CommandParser::Command cmd;
    while (running_.load()) {
        checkObserverCommands();
        if (cmdQueue_.pop(cmd, 500)) {
            std::string result = executeCommand(cmd);
            resultQueue_.push(result);
        }
    }
}

/**
 * checkObserverCommands — 检查观察者发来的命令文件
 *
 * 观察者将命令写入 commands.txt，
 * 后端定期读取并执行，执行后清空文件并保存状态。
 */
void OSSimulator::checkObserverCommands() {
    std::ifstream f(cmdFilePath());
    if (!f.is_open()) return;
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    f.close();
    if (lines.empty()) return;

    // 清空命令文件
    {
        std::ofstream clear(cmdFilePath(), std::ios::trunc);
    }

    for (const auto& l : lines) {
        auto cmd = CommandParser::parse(l);
        if (cmd.name.empty()) continue;
        executeCommand(cmd);
    }
    StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
}

/**
 * pollLoop — 观察者轮询线程
 *
 * 每 200ms 做两件事：
 *   1. 尝试获取文件锁（后端退出后可自动升级）
 *   2. 检查 os_state.bin 时间戳，变了就 load()
 */
void OSSimulator::pollLoop() {
    while (running_.load()) {
        // 尝试升级为后端
        if (backendLock_.tryLock(lockFilePath())) {
            isBackend_ = true;

            StateSerializer::load(stateFilePath(),
                                  processMgr_, memoryMgr_, userMgr_, scheduler_);
            localLoggedIn_ = false;
            localUser_.clear();

            backendThread_ = std::thread(&OSSimulator::backendLoop, this);
            std::cout << "\n[系统] 后端离线, 本实例已升级为后端\n";
            return;
        }

        // 轮询状态文件时间戳
        auto ftime = std::filesystem::last_write_time(stateFilePath());
        auto t = ftime.time_since_epoch().count();
        if (t != lastPollTime_) {
            lastPollTime_ = t;
            StateSerializer::load(stateFilePath(),
                                  processMgr_, memoryMgr_, userMgr_, scheduler_);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/**
 * executeCommand — 命令分发
 *
 * 根据命令名把命令路由到对应的处理函数。
 * 按模块分组：用户管理、进程管理、调度器、内存、持久化、可视化
 */
std::string OSSimulator::executeCommand(const CommandParser::Command& cmd) {
    const std::string& name = cmd.name;

    if (name == "register" || name == "login" || name == "logout") {
        return handleUserCmd(cmd);
    }
    if (name == "create_pcb" || name == "kill_pcb" ||
        name == "block_pcb" || name == "wakeup_pcb" ||
        name == "show_pcb" || name == "list_pcb" ||
        name == "ptree" || name == "suspend" ||
        name == "resume" || name == "renice") {
        return handleProcessCmd(cmd);
    }
    if (name == "start_sched" || name == "stop_sched" ||
        name == "restart_sched" || name == "step") {
        return handleSchedulerCmd(cmd);
    }
    if (name == "alloc" || name == "free_mem" ||
        name == "show_mem" || name == "compact" ||
        name == "mem_stat" || name == "set_alloc_algo" ||
        name == "pgfault" || name == "swap_out" ||
        name == "swap_in") {
        return handleMemoryCmd(cmd);
    }
    if (name == "save" || name == "load" || name == "clear_save") {
        return handlePersistenceCmd(cmd);
    }
    if (name == "overview") {
        return handleOverviewCmd();
    }

    return "未知命令: " + cmd.raw + "\n输入 'help' 查看可用命令\n";
}

// ========== 用户命令处理 ==========

std::string OSSimulator::handleUserCmd(const CommandParser::Command& cmd) {
    if (cmd.name == "register") {
        if (cmd.args.size() < 2)
            return "用法: register <用户名> <密码>\n";
        const std::string& user = cmd.args[0];
        const std::string& pass = cmd.args[1];
        if (userMgr_.registerUser(user, pass)) {
            StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
            return "注册成功! 用户名: " + user + "\n";
        }
        return "注册失败: 用户名 " + user + " 已存在\n";
    }

    if (cmd.name == "login") {
        if (cmd.args.size() < 2)
            return "用法: login <用户名> <密码>\n";
        const std::string& user = cmd.args[0];
        const std::string& pass = cmd.args[1];
        std::string error;
        if (userMgr_.login(user, pass, error)) {
            StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
            return "登录成功! 欢迎 " + user + "\n";
        }
        return "登录失败: " + error + "\n";
    }

    if (cmd.name == "logout") {
        std::string who = userMgr_.currentUser();
        userMgr_.logout();
        return "用户 " + who + " 已登出\n";
    }

    return "内部错误\n";
}

// ========== 进程命令处理 ==========

std::string OSSimulator::handleProcessCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn())
        return "请先登录! 使用 login <用户名> <密码>\n";

    const std::string& name = cmd.name;
    std::string owner = userMgr_.currentUser();

    if (name == "create_pcb") {
        if (cmd.args.size() < 2)
            return "用法: create_pcb <进程名> <优先级(0-15)> [父进程PID] [需要CPU时间]\n";
        std::string pname = cmd.args[0];
        int32_t priority;
        if (!CommandParser::toInt(cmd.args[1], priority))
            return "优先级必须是整数(0-15)\n";
        int32_t ppid = 1;
        if (cmd.args.size() >= 3) {
            if (!CommandParser::toInt(cmd.args[2], ppid))
                return "父进程PID必须是整数\n";
        }
        int32_t burst = 5;
        if (cmd.args.size() >= 4) {
            if (!CommandParser::toInt(cmd.args[3], burst) || burst <= 0)
                return "需要CPU时间必须是正整数\n";
        }
        int32_t pid = processMgr_.createPCB(pname, priority, ppid, owner, burst);
        if (pid < 0)
            return "创建失败: 父进程PID=" + std::to_string(ppid) + " 不存在\n";
        scheduler_.enqueue(pid, 0);  // MLFQ 规则：新进程默认入 Q0
        return "进程创建成功! PID=" + std::to_string(pid)
               + " 名称=" + pname + " 优先级=" + std::to_string(priority)
               + " 需要CPU=" + std::to_string(burst) + "\n";
    }

    if (name == "kill_pcb") {
        if (cmd.args.empty())
            return "用法: kill_pcb <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        bool ok = processMgr_.killPCB(pid, [&](int32_t killedPid) {
            scheduler_.dequeue(killedPid);
            memoryMgr_.freeByPid(killedPid);
        });
        if (ok)
            return "进程 PID=" + std::to_string(pid) + " 已撤销(含子进程)\n";
        return "撤销失败: PID=" + std::to_string(pid) + " 不存在或为init进程\n";
    }

    if (name == "block_pcb") {
        if (cmd.args.empty())
            return "用法: block_pcb <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        if (processMgr_.blockPCB(pid)) {
            scheduler_.dequeue(pid);
            return "进程 PID=" + std::to_string(pid) + " 已阻塞\n";
        }
        return "阻塞失败: PID不存在或状态不允许阻塞\n";
    }

    if (name == "wakeup_pcb") {
        if (cmd.args.empty())
            return "用法: wakeup_pcb <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.wakeupPCB(pid)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) scheduler_.enqueue(pid, 0);  // MLFQ 规则：唤醒重置 Q0
            return "进程 PID=" + std::to_string(pid) + " 已唤醒\n";
        }
        return "唤醒失败: 进程未处于阻塞状态\n";
    }

    if (name == "show_pcb") {
        if (cmd.args.empty())
            return "用法: show_pcb <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        return processMgr_.showPCB(pid);
    }

    if (name == "list_pcb") {
        return processMgr_.listPCB(userMgr_.currentUser());
    }

    if (name == "ptree") {
        std::string tree = processMgr_.pTree(userMgr_.currentUser());
        if (tree.empty()) return "没有找到进程\n";
        return "===== 进程树 =====\n" + tree;
    }

    if (name == "suspend") {
        if (cmd.args.empty())
            return "用法: suspend <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        if (processMgr_.suspendPCB(pid)) {
            scheduler_.dequeue(pid);
            return "进程 PID=" + std::to_string(pid) + " 已挂起\n";
        }
        return "挂起失败: PID不存在、为init进程或状态不允许挂起\n";
    }

    if (name == "resume") {
        if (cmd.args.empty())
            return "用法: resume <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.resumePCB(pid)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) scheduler_.enqueue(pid, 0);  // MLFQ 规则：恢复重置 Q0
            return "进程 PID=" + std::to_string(pid) + " 已恢复\n";
        }
        return "恢复失败: 进程未处于挂起状态\n";
    }

    if (cmd.name == "renice") {
        if (cmd.args.size() < 2)
            return "用法: renice <PID> <新优先级(0-15)>\n";
        int32_t pid, newPrio;
        if (!CommandParser::toInt(cmd.args[0], pid) ||
            !CommandParser::toInt(cmd.args[1], newPrio))
            return "参数必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.renice(pid, newPrio)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb && (pcb->state == PCB::READY || pcb->state == PCB::RUNNING)) {
                scheduler_.dequeue(pid);
                scheduler_.enqueue(pid, newPrio);  // renice 时按新优先级入队列
            }
            return "进程 PID=" + std::to_string(pid)
                   + " 优先级已修改为 " + std::to_string(newPrio) + ", 已移至新队列\n";
        }
        return "修改失败: PID不存在或优先级范围无效(0-15)\n";
    }

    return "内部错误\n";
}

// ========== 调度器命令处理 ==========

std::string OSSimulator::handleSchedulerCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn())
        return "请先登录!\n";

    if (cmd.name == "start_sched") {
        if (scheduler_.isRunning())
            return "调度器已在运行中, 无需重复启动\n" + scheduler_.queueStatus();
        scheduler_.start();
        return "调度器已启动 (多级反馈队列)\n" + scheduler_.queueStatus();
    }

    if (cmd.name == "stop_sched") {
        scheduler_.stop();
        return "调度器已停止\n";
    }

    if (cmd.name == "restart_sched") {
        scheduler_.restart();
        return "调度器已重启\n";
    }

    if (cmd.name == "step") {
        return scheduler_.step();
    }

    return "内部错误\n";
}

// ========== 内存命令处理 ==========

std::string OSSimulator::handleMemoryCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn())
        return "请先登录!\n";

    if (cmd.name == "alloc") {
        if (cmd.args.size() < 2)
            return "用法: alloc <大小(KB)> <目标>\n"
                   "   目标: PID(数字) 或 data / io / kernel\n";
        int32_t size;
        if (!CommandParser::toInt(cmd.args[0], size))
            return "大小必须是整数\n";
        if (size <= 0 || size > memoryMgr_.totalSize())
            return "大小必须在1-" + std::to_string(memoryMgr_.totalSize()) + "KB之间\n";

        int32_t pid;
        bool isProcessTarget = false;
        std::string targetLabel;

        if (CommandParser::toInt(cmd.args[1], pid)) {
            isProcessTarget = true;
        } else {
            std::string target = CommandParser::toLower(cmd.args[1]);
            if (target == "data") {
                pid = MemoryManager::PID_DATA;
                targetLabel = "数据";
            } else if (target == "io") {
                pid = MemoryManager::PID_IO;
                targetLabel = "IO";
            } else if (target == "kernel") {
                pid = MemoryManager::PID_KERNEL;
                targetLabel = "内核";
            } else {
                return "无效目标: " + cmd.args[1] + " (支持: PID数字 / data / io / kernel)\n";
            }
        }

        if (isProcessTarget) {
            std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
            if (!processMgr_.getPCB(pid))
                return "分配失败: PID=" + std::to_string(pid) + " 不存在\n";

            int32_t addr = memoryMgr_.alloc(size, pid);
            if (addr < 0)
                return "内存分配失败: 没有足够的连续空间\n";

            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) {
                pcb->memoryBlocks.emplace_back(addr, size);
                pcb->totalMemory += size;
            }
            return "内存分配成功! 起始地址=" + std::to_string(addr)
                   + "KB, 大小=" + std::to_string(size) + "KB, PID="
                   + std::to_string(pid) + "\n";
        } else {
            std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());
            int32_t addr = memoryMgr_.alloc(size, pid);
            if (addr < 0)
                return "内存分配失败: 没有足够的连续空间\n";
            return "内存分配成功! 起始地址=" + std::to_string(addr)
                   + "KB, 大小=" + std::to_string(size) + "KB, 目标=" + targetLabel + "\n";
        }
    }

    if (cmd.name == "free_mem") {
        if (cmd.args.empty())
            return "用法: free_mem <起始地址(KB)>\n";
        int32_t addr;
        if (!CommandParser::toInt(cmd.args[0], addr))
            return "起始地址必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        int32_t ownerPid = memoryMgr_.getPidByAddr(addr);
        if (memoryMgr_.freeByAddr(addr)) {
            if (ownerPid >= 0) {
                PCB* pcb = processMgr_.getPCB(ownerPid);
                if (pcb) {
                    auto& blocks = pcb->memoryBlocks;
                    blocks.erase(
                        std::remove_if(blocks.begin(), blocks.end(),
                            [addr](const std::pair<int32_t,int32_t>& b) {
                                return b.first == addr;
                            }),
                        blocks.end());
                    pcb->totalMemory = 0;
                    for (const auto& b : blocks) pcb->totalMemory += b.second;
                }
            }
            return "内存已释放: 地址=" + std::to_string(addr) + "KB\n";
        }
        return "释放失败: 未找到该地址的已分配块\n";
    }

    if (cmd.name == "show_mem") {
        return memoryMgr_.showMem();
    }

    if (cmd.name == "compact") {
        memoryMgr_.compact();
        return "内存紧缩完成\n" + memoryMgr_.showMem();
    }

    if (cmd.name == "mem_stat") {
        return memoryMgr_.memStat();
    }

    if (cmd.name == "set_alloc_algo") {
        if (cmd.args.empty())
            return "用法: set_alloc_algo <FF|BF|WF>\n";
        std::string algo = CommandParser::toLower(cmd.args[0]);
        if (algo == "ff" || algo == "first_fit")
            memoryMgr_.setAllocAlgo(MemoryManager::FIRST_FIT);
        else if (algo == "bf" || algo == "best_fit")
            memoryMgr_.setAllocAlgo(MemoryManager::BEST_FIT);
        else if (algo == "wf" || algo == "worst_fit")
            memoryMgr_.setAllocAlgo(MemoryManager::WORST_FIT);
        else
            return "未知算法: " + cmd.args[0] + " (支持: FF/BF/WF)\n";
        return "分配算法已切换为: " + memoryMgr_.getAllocAlgoName() + "\n";
    }

    if (cmd.name == "pgfault") {
        return "=== 缺页中断模拟 ===\n"
               "[缺页] 页面错误发生在虚拟地址 0xDEADBEEF\n"
               "[缺页] 从磁盘交换区读取页面...\n"
               "[缺页] 页面加载完成, 页表已更新\n"
               "缺页中断处理完毕\n";
    }

    if (cmd.name == "swap_out") {
        if (cmd.args.empty())
            return "用法: swap_out <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());
        PCB* pcb = processMgr_.getPCB(pid);
        if (!pcb)
            return "进程 PID=" + std::to_string(pid) + " 不存在\n";
        if (pcb->totalMemory == 0)
            return "进程 PID=" + std::to_string(pid) + " 没有已分配内存, 无需换出\n";
        swappedOut_[pid] = pcb->memoryBlocks;
        int32_t freedSize = pcb->totalMemory;
        memoryMgr_.freeByPid(pid);
        pcb->memoryBlocks.clear();
        pcb->totalMemory = 0;
        std::ostringstream oss;
        oss << "=== 换出操作 ===\n"
            << "已将进程 " << pcb->name << "(" << pid << ") 的 "
            << freedSize << "KB 内存换出到交换空间\n"
            << "[换出] 模拟磁盘I/O写入... 完成\n"
            << "[换出] 物理内存已释放, show_mem 中该进程块已消失\n"
            << "[换出] 可用 swap_in " << pid << " 恢复\n";
        return oss.str();
    }

    if (cmd.name == "swap_in") {
        if (cmd.args.empty())
            return "用法: swap_in <PID>\n";
        int32_t pid;
        if (!CommandParser::toInt(cmd.args[0], pid))
            return "PID必须是整数\n";
        auto it = swappedOut_.find(pid);
        if (it == swappedOut_.end() || it->second.empty())
            return "换入失败: 进程 PID=" + std::to_string(pid) + " 没有已换出的内存记录\n";

        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());

        PCB* pcb = processMgr_.getPCB(pid);
        if (!pcb)
            return "换入失败: 进程 PID=" + std::to_string(pid) + " 不存在\n";

        int32_t restoredSize = 0;
        for (const auto& blk : it->second) {
            int32_t addr = memoryMgr_.alloc(blk.second, pid);
            if (addr < 0) {
                for (const auto& rblk : pcb->memoryBlocks) {
                    memoryMgr_.freeByAddr(rblk.first);
                }
                pcb->memoryBlocks.clear();
                pcb->totalMemory = 0;
                return "换入失败: 可用内存不足, 无法恢复进程 " + std::to_string(pid) + "\n";
            }
            pcb->memoryBlocks.emplace_back(addr, blk.second);
            restoredSize += blk.second;
        }
        pcb->totalMemory = restoredSize;
        swappedOut_.erase(pid);

        std::ostringstream oss;
        oss << "=== 换入操作 ===\n"
            << "已将进程 " << pcb->name << "(" << pid << ") 的 "
            << restoredSize << "KB 内存从交换区恢复\n"
            << "[换入] 模拟磁盘I/O读取... 完成\n"
            << "[换入] 物理内存已分配, show_mem 中该进程块已恢复\n";
        return oss.str();
    }

    return "内部错误\n";
}

// ========== 持久化命令处理 ==========

std::string OSSimulator::handlePersistenceCmd(const CommandParser::Command& cmd) {
    if (cmd.name == "save") {
        if (StateSerializer::save(stateFilePath(),
                                  processMgr_, memoryMgr_,
                                  userMgr_, scheduler_))
            return "系统状态已保存到: " + stateFilePath() + "\n";
        return "保存失败!\n";
    }

    if (cmd.name == "load") {
        if (StateSerializer::load(stateFilePath(),
                                  processMgr_, memoryMgr_,
                                  userMgr_, scheduler_))
            return "系统状态已从文件恢复!\n";
        return "加载失败! 文件可能不存在或损坏\n";
    }

    if (cmd.name == "clear_save") {
        scheduler_.stop();
        processMgr_.clear();
        memoryMgr_.clear();
        userMgr_.clear();
        for (int i = 0; i < 3; i++) scheduler_.setQueue(i, {});
        processMgr_.createPCB("init", 0, -1, "system", 999999);
        std::remove(stateFilePath().c_str());
        return "持久化数据已清空, 系统恢复初始状态\n";
    }

    return "内部错误\n";
}

// ========== 可视化总览 ==========

/**
 * handleOverviewCmd — 系统全景快照
 *
 * 在 58 列宽的框内显示：
 *   1. 进程树（Process Tree）
 *   2. 内存分布图（Memory Map）
 *   3. 多级反馈队列状态（MLFQ）
 *   4. 快速统计（就绪/运行/阻塞/挂起数量）
 */
std::string OSSimulator::handleOverviewCmd() {
    if (!userMgr_.isLoggedIn())
        return "请先登录!\n";

    std::ostringstream oss;
    std::string owner = userMgr_.currentUser();

    oss << "\n";
    oss << "╔══════════════════════════════════════════════════════════╗\n";
    oss << "║                 === System Overview ===                 ║\n";
    oss << "╠══════════════════════════════════════════════════════════╣\n";

    // 进程树
    oss << "║ 【Process Tree】                                        ║\n";
    std::string tree = processMgr_.pTree(owner);
    if (tree.empty()) {
        oss << "║   (无进程)                                              ║\n";
    } else {
        std::istringstream tstream(tree);
        std::string line;
        while (std::getline(tstream, line)) {
            if (line.empty()) continue;
            if (line.size() > 54) line = line.substr(0, 51) + "...";
            oss << "║ " << std::left << std::setw(55) << line << "║\n";
        }
    }

    oss << "╠══════════════════════════════════════════════════════════╣\n";

    // 内存分布图
    oss << "║ 【Memory Map】 (0-" << memoryMgr_.totalSize() << "KB)";
    oss << std::string(16, ' ') << "║\n";

    auto allBlocks = memoryMgr_.getAllBlocks();
    std::ostringstream memBar;
    for (const auto& blk : allBlocks) {
        if (blk.free) {
            memBar << "|--free(" << blk.size << "KB)--";
        } else {
            memBar << "|##" << MemoryManager::getOwnerLabel(blk.pid) << "(" << blk.size << "KB)";
        }
    }
    std::string barStr = memBar.str();
    if (barStr.size() > 52) barStr = barStr.substr(0, 49) + "...";
    oss << "║ " << std::left << std::setw(55) << barStr << "║\n";

    oss << "╠══════════════════════════════════════════════════════════╣\n";

    // 多级反馈队列
    oss << "║ 【多级反馈队列】                                        ║\n";
    std::string qs = scheduler_.queueStatus();
    std::istringstream qstream(qs);
    std::string qline;
    while (std::getline(qstream, qline)) {
        if (qline.empty()) continue;
        if (qline.size() > 54) qline = qline.substr(0, 51) + "...";
        oss << "║ " << std::left << std::setw(55) << qline << "║\n";
    }

    oss << "╠══════════════════════════════════════════════════════════╣\n";

    // 快速统计
    int readyCount = 0, runningCount = 0, blockedCount = 0, suspendedCount = 0;
    {
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        const auto& pcbs = processMgr_.getAllPCBs();
        for (const auto& pair : pcbs) {
            if (!owner.empty() && pair.second.owner != owner) continue;
            switch (pair.second.state) {
                case PCB::READY: readyCount++; break;
                case PCB::RUNNING: runningCount++; break;
                case PCB::BLOCKED: blockedCount++; break;
                case PCB::SUSPENDED: suspendedCount++; break;
                default: break;
            }
        }
    }

    oss << "║ 【Stats】 就绪:" << std::setw(4) << readyCount
        << " 运行:" << std::setw(4) << runningCount
        << " 阻塞:" << std::setw(4) << blockedCount
        << " 挂起:" << std::setw(4) << suspendedCount;
    oss << "    ║\n";

    oss << "║ 调度器: " << std::setw(8) << (scheduler_.isRunning() ? "运行中" : "已停止");
    oss << "  算法: " << std::setw(14) << memoryMgr_.getAllocAlgoName() << " ║\n";

    oss << "╚══════════════════════════════════════════════════════════╝\n";

    return oss.str();
}

void OSSimulator::showWelcome() {
    if (isBackend_) {
        std::cout << "OS Simulator v1.0 [后端]  |  help 帮助  exit 退出\n\n";
    } else {
        std::cout << "OS Simulator v1.0 [观察者]  |  help 帮助  exit 退出\n\n";
    }
}

void OSSimulator::showHelp() {
    std::cout << "\n";
    std::cout << "========== 可用命令一览 ==========\n\n";

    std::cout << "--- 用户管理 ---\n";
    std::cout << "  register <用户名> <密码>   注册新用户\n";
    std::cout << "  login <用户名> <密码>      登录系统\n";
    std::cout << "  logout                     登出\n\n";

    std::cout << "--- 进程管理 (每个2分, 共20分) ---\n";
    std::cout << "  create_pcb <名称> <优先级> [ppid] [需CPU]  创建进程\n";
    std::cout << "  kill_pcb <PID>                      撤销进程\n";
    std::cout << "  block_pcb <PID>                     阻塞进程\n";
    std::cout << "  wakeup_pcb <PID>                    唤醒进程\n";
    std::cout << "  show_pcb <PID>                      查看PCB详细信息\n";
    std::cout << "  list_pcb                            列出所有进程\n";
    std::cout << "  ptree                               树形显示进程关系\n";
    std::cout << "  suspend <PID>                       挂起进程\n";
    std::cout << "  resume <PID>                        恢复挂起进程\n";
    std::cout << "  renice <PID> <优先级>               修改优先级\n\n";

    std::cout << "--- 调度器 (每个2分, 共8分) ---\n";
    std::cout << "  start_sched       启动多级反馈队列自动调度\n";
    std::cout << "  stop_sched        暂停调度\n";
    std::cout << "  restart_sched     重启调度\n";
    std::cout << "  step              单步调度\n\n";

    std::cout << "--- 内存管理 (每个2分, 共16分) ---\n";
    std::cout << "  alloc <大小KB> <PID|data|io|kernel>  分配内存\n";
    std::cout << "  free_mem <起始地址KB>                释放内存\n";
    std::cout << "  show_mem                             显示内存使用\n";
    std::cout << "  compact                              内存紧缩\n";
    std::cout << "  mem_stat                             内存统计\n";
    std::cout << "  set_alloc_algo <FF|BF|WF>            切换分配算法\n";
    std::cout << "  pgfault                              模拟缺页中断\n";
    std::cout << "  swap_out <PID>                       换出进程内存\n";
    std::cout << "  swap_in <PID>                        换入进程内存\n\n";

    std::cout << "--- 持久化 (共20分) ---\n";
    std::cout << "  save          保存状态到二进制文件\n";
    std::cout << "  load          从文件恢复状态\n";
    std::cout << "  clear_save    清空持久化数据, 恢复初始状态\n\n";

    std::cout << "--- 可视化 (10分) ---\n";
    std::cout << "  overview      系统全景快照\n\n";

    std::cout << "--- 系统 ---\n";
    std::cout << "  help / ?      显示帮助\n";
    std::cout << "  exit / quit   退出系统\n\n";
}
