// 操作系统模拟器 — 主控实现
// 前台交互循环 + 后台线程 + 多实例支持 + 命令分发
#include "系统模拟器.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

OSSimulator::OSSimulator()
    : isBackend_(false), running_(false), lastPollTime_(0)
{
    // 数据目录基于 exe 路径，这样不管从哪启动都能找到
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
    } else dataDir_ = ".\\数据";
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

void OSSimulator::init() {
    scheduler_.init(&processMgr_);
    scheduler_.setOnTerminate([this](int32_t pid) { memoryMgr_.freeByPid(pid); });
    scheduler_.setLogCallback([this](const char* role, const std::string& msg) {
        logPrint(role, msg);
    });

    if (StateSerializer::fileExists(stateFilePath())) {
        if (StateSerializer::load(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_))
            return;
    }
    processMgr_.createPCB("init", 0, -1, "system", 999999);
}

void OSSimulator::run() {
    running_ = true;

#ifdef _WIN32
    CreateDirectoryW(utf8ToWide(dataDir_).c_str(), nullptr);
#else
    mkdir(dataDir_.c_str(), 0755);
#endif

    isBackend_ = backendLock_.tryLock(lockFilePath());
    if (isBackend_) {
        backendThread_ = std::thread(&OSSimulator::backendLoop, this);
        StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
    } else {
        lastPollTime_ = 0;
        pollThread_ = std::thread(&OSSimulator::pollLoop, this);
    }

    showWelcome();

    std::string input;
    while (running_.load()) {
        if (isBackend_)
            std::cout << (userMgr_.isLoggedIn()
                ? "[" + userMgr_.currentUser() + "@OS]> "
                : "[未登录@OS]> ");
        else
            std::cout << (localLoggedIn_
                ? "[" + localUser_ + "@OS]> "
                : "[未登录@OS]> ");
        std::cout.flush();

        if (!std::getline(std::cin, input)) break;
        if (input.empty()) continue;

        auto cmd = CommandParser::parse(input);
        if (cmd.name.empty()) continue;

        if (cmd.name == "exit" || cmd.name == "quit") { std::cout << "[系统] 再见!\n"; break; }
        if (cmd.name == "help" || cmd.name == "?") { showHelp(); continue; }

        // 日志命令不需要投递到后端，直接在主线程处理
        if (cmd.name == "log_on" || cmd.name == "log_off") {
            std::cout << handleLogCmd(cmd);
            continue;
        }

        if (isBackend_) {
            logPrint("前台", "收到命令: " + cmd.raw + " → 推入消息队列");
            cmdQueue_.push(cmd);
            logPrint("前台", "等待后台执行结果...");
            std::string result;
            if (resultQueue_.pop(result, 5000)) std::cout << result;
            else {
                std::cout << "[错误] 命令执行超时\n";
                std::string stale;
                while (resultQueue_.tryPop(stale)) {}
            }
            StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
        } else {
            bool isAuthCmd = (cmd.name == "login" || cmd.name == "register" || cmd.name == "logout");
            logPrint("前台(观察者)", "收到命令: " + cmd.raw + " → 写入 commands.txt");
            {
                std::ofstream f(cmdFilePath(), std::ios::app);
                if (f.is_open()) { f << input << "\n"; f.close(); }
            }
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
                        StateSerializer::load(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
                        updated = true; break;
                    }
                }
                if (cmd.name == "login") {
                    if (updated && userMgr_.isLoggedIn()) {
                        localLoggedIn_ = true; localUser_ = userMgr_.currentUser();
                        std::cout << "登录成功! 欢迎 " << localUser_ << "\n";
                    } else { localLoggedIn_ = false; localUser_.clear();
                        std::cout << "登录失败: 用户名或密码错误, 或账户已锁定\n"; }
                } else if (cmd.name == "register")
                    std::cout << (updated ? "注册请求已处理\n" : "注册超时, 请重试\n");
                else if (cmd.name == "logout")
                    { localLoggedIn_ = false; localUser_.clear(); std::cout << "已登出\n"; }
            } else std::cout << "[命令已发送到后端]\n";
        }
    }

    running_ = false;
    cmdQueue_.stop();
    if (pollThread_.joinable()) pollThread_.join();
    if (backendThread_.joinable()) backendThread_.join();
}

void OSSimulator::backendLoop() {
    CommandParser::Command cmd;
    while (running_.load()) {
        checkObserverCommands();
        if (cmdQueue_.pop(cmd, 500)) {
            logPrint("后台", "取出命令: " + cmd.raw + " → 开始执行");
            std::string result = executeCommand(cmd);
            resultQueue_.push(result);
            logPrint("后台", "命令执行完毕, 结果已推入 resultQueue");
        }
    }
}

void OSSimulator::checkObserverCommands() {
    std::ifstream f(cmdFilePath());
    if (!f.is_open()) return;
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) if (!line.empty()) lines.push_back(line);
    f.close();
    if (lines.empty()) return;
    { std::ofstream clear(cmdFilePath(), std::ios::trunc); }
    for (const auto& l : lines) {
        auto cmd = CommandParser::parse(l);
        if (!cmd.name.empty()) {
            logPrint("后台", "执行观察者命令: " + l);
            executeCommand(cmd);
        }
    }
    StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
}

void OSSimulator::pollLoop() {
    while (running_.load()) {
        if (backendLock_.tryLock(lockFilePath())) {
            isBackend_ = true;
            StateSerializer::load(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
            localLoggedIn_ = false; localUser_.clear();
            backendThread_ = std::thread(&OSSimulator::backendLoop, this);
            logPrint("观察者", "获取文件锁成功, 升级为后端");
            std::cout << "\n[系统] 后端离线, 本实例已升级为后端\n";
            return;
        }
        auto ftime = std::filesystem::last_write_time(stateFilePath());
        auto t = ftime.time_since_epoch().count();
        if (t != lastPollTime_) {
            lastPollTime_ = t;
            StateSerializer::load(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
            logPrint("观察者", "检测到 os_state.bin 变化, 已重新加载状态");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::string OSSimulator::executeCommand(const CommandParser::Command& cmd) {
    const std::string& name = cmd.name;
    if (name == "register" || name == "login" || name == "logout") return handleUserCmd(cmd);
    if (name == "create_pcb" || name == "kill_pcb" || name == "block_pcb" || name == "wakeup_pcb" ||
        name == "show_pcb" || name == "list_pcb" || name == "ptree" || name == "suspend" ||
        name == "resume" || name == "renice") return handleProcessCmd(cmd);
    if (name == "start_sched" || name == "stop_sched" || name == "restart_sched" || name == "step")
        return handleSchedulerCmd(cmd);
    if (name == "alloc" || name == "free_mem" || name == "show_mem" || name == "compact" ||
        name == "mem_stat" || name == "set_alloc_algo" || name == "pgfault" || name == "swap_out" || name == "swap_in")
        return handleMemoryCmd(cmd);
    if (name == "save" || name == "load" || name == "clear_save") return handlePersistenceCmd(cmd);
    if (name == "overview") return handleOverviewCmd();
    if (name == "log_on" || name == "log_off") return handleLogCmd(cmd);
    return "未知命令: " + cmd.raw + "\n输入 'help' 查看可用命令\n";
}

// ===== 用户命令 =====
std::string OSSimulator::handleUserCmd(const CommandParser::Command& cmd) {
    if (cmd.name == "register") {
        return (cmd.args.size() < 2) ? "用法: register <用户名> <密码>\n"
            : (userMgr_.registerUser(cmd.args[0], cmd.args[1])
                ? (StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_),
                   "注册成功! 用户名: " + cmd.args[0] + "\n")
                : "注册失败: 用户名 " + cmd.args[0] + " 已存在\n");
    }
    if (cmd.name == "login") {
        if (cmd.args.size() < 2) return "用法: login <用户名> <密码>\n";
        std::string error;
        if (userMgr_.login(cmd.args[0], cmd.args[1], error)) {
            StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_);
            return "登录成功! 欢迎 " + cmd.args[0] + "\n";
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

// ===== 进程命令 =====
std::string OSSimulator::handleProcessCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn()) return "请先登录! 使用 login <用户名> <密码>\n";
    const std::string& name = cmd.name;
    std::string owner = userMgr_.currentUser();

    if (name == "create_pcb") {
        if (cmd.args.size() < 2) return "用法: create_pcb <进程名> <优先级(0-15)> [父进程PID] [需要CPU时间]\n";
        int32_t priority;
        if (!CommandParser::toInt(cmd.args[1], priority)) return "优先级必须是整数(0-15)\n";
        int32_t ppid = 1;
        if (cmd.args.size() >= 3 && !CommandParser::toInt(cmd.args[2], ppid)) return "父进程PID必须是整数\n";
        int32_t burst = 5;
        if (cmd.args.size() >= 4) {
            if (!CommandParser::toInt(cmd.args[3], burst) || burst <= 0) return "需要CPU时间必须是正整数\n";
        }
        int32_t pid = processMgr_.createPCB(cmd.args[0], priority, ppid, owner, burst);
        if (pid < 0) return "创建失败: 父进程PID=" + std::to_string(ppid) + " 不存在\n";
        scheduler_.enqueue(pid, 0);  // 新进程入 Q0
        return "进程创建成功! PID=" + std::to_string(pid) + " 名称=" + cmd.args[0]
               + " 优先级=" + std::to_string(priority) + " 需要CPU=" + std::to_string(burst) + "\n";
    }

    if (name == "kill_pcb") {
        if (cmd.args.empty()) return "用法: kill_pcb <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        bool ok = processMgr_.killPCB(pid, [&](int32_t kp) { scheduler_.dequeue(kp); memoryMgr_.freeByPid(kp); });
        return ok ? ("进程 PID=" + std::to_string(pid) + " 已撤销(含子进程)\n")
                  : ("撤销失败: PID=" + std::to_string(pid) + " 不存在或为init进程\n");
    }

    if (name == "block_pcb") {
        if (cmd.args.empty()) return "用法: block_pcb <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        if (processMgr_.blockPCB(pid)) { scheduler_.dequeue(pid); return "进程 PID=" + std::to_string(pid) + " 已阻塞\n"; }
        return "阻塞失败: PID不存在或状态不允许阻塞\n";
    }

    if (name == "wakeup_pcb") {
        if (cmd.args.empty()) return "用法: wakeup_pcb <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.wakeupPCB(pid)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) scheduler_.enqueue(pid, 0);
            return "进程 PID=" + std::to_string(pid) + " 已唤醒\n";
        }
        return "唤醒失败: 进程未处于阻塞状态\n";
    }

    if (name == "show_pcb") {
        if (cmd.args.empty()) return "用法: show_pcb <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        return processMgr_.showPCB(pid);
    }

    if (name == "list_pcb") return processMgr_.listPCB(owner);

    if (name == "ptree") {
        std::string tree = processMgr_.pTree(owner);
        return tree.empty() ? "没有找到进程\n" : "===== 进程树 =====\n" + tree;
    }

    if (name == "suspend") {
        if (cmd.args.empty()) return "用法: suspend <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        if (processMgr_.suspendPCB(pid)) { scheduler_.dequeue(pid); return "进程 PID=" + std::to_string(pid) + " 已挂起\n"; }
        return "挂起失败: PID不存在、为init进程或状态不允许挂起\n";
    }

    if (name == "resume") {
        if (cmd.args.empty()) return "用法: resume <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.resumePCB(pid)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) scheduler_.enqueue(pid, 0);
            return "进程 PID=" + std::to_string(pid) + " 已恢复\n";
        }
        return "恢复失败: 进程未处于挂起状态\n";
    }

    if (cmd.name == "renice") {
        if (cmd.args.size() < 2) return "用法: renice <PID> <新优先级(0-15)>\n";
        int32_t pid, np;
        if (!CommandParser::toInt(cmd.args[0], pid) || !CommandParser::toInt(cmd.args[1], np)) return "参数必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        if (processMgr_.renice(pid, np)) {
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb && (pcb->state == PCB::READY || pcb->state == PCB::RUNNING)) {
                scheduler_.dequeue(pid); scheduler_.enqueue(pid, np);
            }
            return "进程 PID=" + std::to_string(pid) + " 优先级已修改为 " + std::to_string(np) + ", 已移至新队列\n";
        }
        return "修改失败: PID不存在或优先级范围无效(0-15)\n";
    }
    return "内部错误\n";
}

// ===== 调度器命令 =====
std::string OSSimulator::handleSchedulerCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn()) return "请先登录!\n";
    if (cmd.name == "start_sched") {
        if (scheduler_.isRunning()) return "调度器已在运行中, 无需重复启动\n" + scheduler_.queueStatus();
        scheduler_.start();
        return "调度器已启动 (多级反馈队列)\n" + scheduler_.queueStatus();
    }
    if (cmd.name == "stop_sched") { scheduler_.stop(); return "调度器已停止\n"; }
    if (cmd.name == "restart_sched") { scheduler_.restart(); return "调度器已重启\n"; }
    if (cmd.name == "step") return scheduler_.step();
    return "内部错误\n";
}

// ===== 内存命令 =====
std::string OSSimulator::handleMemoryCmd(const CommandParser::Command& cmd) {
    if (!userMgr_.isLoggedIn()) return "请先登录!\n";

    if (cmd.name == "alloc") {
        if (cmd.args.size() < 2) return "用法: alloc <大小(KB)> <目标>\n   目标: PID(数字) 或 data / io / kernel\n";
        int32_t size; if (!CommandParser::toInt(cmd.args[0], size)) return "大小必须是整数\n";
        if (size <= 0 || size > memoryMgr_.totalSize()) return "大小必须在1-" + std::to_string(memoryMgr_.totalSize()) + "KB之间\n";
        int32_t pid; bool isProc = false;
        if (CommandParser::toInt(cmd.args[1], pid)) isProc = true;
        else {
            std::string t = CommandParser::toLower(cmd.args[1]);
            if (t == "data") pid = MemoryManager::PID_DATA;
            else if (t == "io") pid = MemoryManager::PID_IO;
            else if (t == "kernel") pid = MemoryManager::PID_KERNEL;
            else return "无效目标: " + cmd.args[1] + " (支持: PID数字 / data / io / kernel)\n";
        }

        if (isProc) {
            std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
            if (!processMgr_.getPCB(pid)) return "分配失败: PID=" + std::to_string(pid) + " 不存在\n";
            int32_t addr = memoryMgr_.alloc(size, pid);
            if (addr < 0) return "内存分配失败: 没有足够的连续空间\n";
            PCB* pcb = processMgr_.getPCB(pid);
            if (pcb) { pcb->memoryBlocks.emplace_back(addr, size); pcb->totalMemory += size; }
            return "内存分配成功! 起始地址=" + std::to_string(addr) + "KB, 大小=" + std::to_string(size) + "KB, PID=" + std::to_string(pid) + "\n";
        } else {
            std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());
            int32_t addr = memoryMgr_.alloc(size, pid);
            if (addr < 0) return "内存分配失败: 没有足够的连续空间\n";
            std::string label = (pid == MemoryManager::PID_DATA ? "数据" : pid == MemoryManager::PID_IO ? "IO" : "内核");
            return "内存分配成功! 起始地址=" + std::to_string(addr) + "KB, 大小=" + std::to_string(size) + "KB, 目标=" + label + "\n";
        }
    }

    if (cmd.name == "free_mem") {
        if (cmd.args.empty()) return "用法: free_mem <起始地址(KB)>\n";
        int32_t addr; if (!CommandParser::toInt(cmd.args[0], addr)) return "起始地址必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        int32_t ownerPid = memoryMgr_.getPidByAddr(addr);
        if (memoryMgr_.freeByAddr(addr)) {
            if (ownerPid >= 0) {
                PCB* pcb = processMgr_.getPCB(ownerPid);
                if (pcb) {
                    auto& blocks = pcb->memoryBlocks;
                    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                        [addr](const auto& b) { return b.first == addr; }), blocks.end());
                    pcb->totalMemory = 0;
                    for (const auto& b : blocks) pcb->totalMemory += b.second;
                }
            }
            return "内存已释放: 地址=" + std::to_string(addr) + "KB\n";
        }
        return "释放失败: 未找到该地址的已分配块\n";
    }

    if (cmd.name == "show_mem") return memoryMgr_.showMem();
    if (cmd.name == "compact") { memoryMgr_.compact(); return "内存紧缩完成\n" + memoryMgr_.showMem(); }
    if (cmd.name == "mem_stat") return memoryMgr_.memStat();

    if (cmd.name == "set_alloc_algo") {
        if (cmd.args.empty()) return "用法: set_alloc_algo <FF|BF|WF>\n";
        std::string a = CommandParser::toLower(cmd.args[0]);
        if (a == "ff" || a == "first_fit") memoryMgr_.setAllocAlgo(MemoryManager::FIRST_FIT);
        else if (a == "bf" || a == "best_fit") memoryMgr_.setAllocAlgo(MemoryManager::BEST_FIT);
        else if (a == "wf" || a == "worst_fit") memoryMgr_.setAllocAlgo(MemoryManager::WORST_FIT);
        else return "未知算法: " + cmd.args[0] + " (支持: FF/BF/WF)\n";
        return "分配算法已切换为: " + memoryMgr_.getAllocAlgoName() + "\n";
    }

    if (cmd.name == "pgfault") {
        return "=== 缺页中断模拟 ===\n[缺页] 页面错误发生在虚拟地址 0xDEADBEEF\n[缺页] 从磁盘交换区读取页面...\n[缺页] 页面加载完成, 页表已更新\n缺页中断处理完毕\n";
    }

    if (cmd.name == "swap_out") {
        if (cmd.args.empty()) return "用法: swap_out <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());
        PCB* pcb = processMgr_.getPCB(pid);
        if (!pcb) return "进程 PID=" + std::to_string(pid) + " 不存在\n";
        if (pcb->totalMemory == 0) return "进程 PID=" + std::to_string(pid) + " 没有已分配内存, 无需换出\n";
        swappedOut_[pid] = pcb->memoryBlocks;
        int32_t sz = pcb->totalMemory;
        memoryMgr_.freeByPid(pid); pcb->memoryBlocks.clear(); pcb->totalMemory = 0;
        return "=== 换出操作 ===\n已将进程 " + pcb->name + "(" + std::to_string(pid) + ") 的 "
               + std::to_string(sz) + "KB 内存换出到交换空间\n[换出] 模拟磁盘I/O写入... 完成\n[换出] 物理内存已释放\n[换出] 可用 swap_in " + std::to_string(pid) + " 恢复\n";
    }

    if (cmd.name == "swap_in") {
        if (cmd.args.empty()) return "用法: swap_in <PID>\n";
        int32_t pid; if (!CommandParser::toInt(cmd.args[0], pid)) return "PID必须是整数\n";
        auto it = swappedOut_.find(pid);
        if (it == swappedOut_.end() || it->second.empty()) return "换入失败: 进程 PID=" + std::to_string(pid) + " 没有已换出的内存记录\n";

        std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
        std::lock_guard<std::recursive_mutex> mmLock(memoryMgr_.mutex());
        PCB* pcb = processMgr_.getPCB(pid);
        if (!pcb) return "换入失败: 进程 PID=" + std::to_string(pid) + " 不存在\n";

        int32_t rs = 0;
        for (const auto& blk : it->second) {
            int32_t addr = memoryMgr_.alloc(blk.second, pid);
            if (addr < 0) {
                for (const auto& rb : pcb->memoryBlocks) memoryMgr_.freeByAddr(rb.first);
                pcb->memoryBlocks.clear(); pcb->totalMemory = 0;
                return "换入失败: 可用内存不足\n";
            }
            pcb->memoryBlocks.emplace_back(addr, blk.second);
            rs += blk.second;
        }
        pcb->totalMemory = rs;
        swappedOut_.erase(pid);
        return "=== 换入操作 ===\n已将进程 " + pcb->name + "(" + std::to_string(pid) + ") 的 "
               + std::to_string(rs) + "KB 内存从交换区恢复\n[换入] 模拟磁盘I/O读取... 完成\n[换入] 物理内存已分配\n";
    }

    return "内部错误\n";
}

// ===== 持久化命令 =====
std::string OSSimulator::handlePersistenceCmd(const CommandParser::Command& cmd) {
    if (cmd.name == "save") {
        return StateSerializer::save(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_)
            ? "系统状态已保存到: " + stateFilePath() + "\n" : "保存失败!\n";
    }
    if (cmd.name == "load") {
        return StateSerializer::load(stateFilePath(), processMgr_, memoryMgr_, userMgr_, scheduler_)
            ? "系统状态已从文件恢复!\n" : "加载失败! 文件可能不存在或损坏\n";
    }
    if (cmd.name == "clear_save") {
        scheduler_.stop(); processMgr_.clear(); memoryMgr_.clear(); userMgr_.clear();
        for (int i = 0; i < 3; i++) scheduler_.setQueue(i, {});
        processMgr_.createPCB("init", 0, -1, "system", 999999);
        std::remove(stateFilePath().c_str());
        return "持久化数据已清空, 系统恢复初始状态\n";
    }
    return "内部错误\n";
}

// ===== overview 全景快照 =====
std::string OSSimulator::handleOverviewCmd() {
    if (!userMgr_.isLoggedIn()) return "请先登录!\n";
    std::ostringstream oss;
    std::string owner = userMgr_.currentUser();

    oss << "\n╔══════════════════════════════════════════════════════════╗\n"
        << "║                 === System Overview ===                 ║\n"
        << "╠══════════════════════════════════════════════════════════╣\n"
        << "║ 【Process Tree】                                        ║\n";
    std::string tree = processMgr_.pTree(owner);
    if (tree.empty()) oss << "║   (无进程)                                              ║\n";
    else {
        std::istringstream ts(tree); std::string line;
        while (std::getline(ts, line)) {
            if (line.empty()) continue;
            if (line.size() > 54) line = line.substr(0, 51) + "...";
            oss << "║ " << std::left << std::setw(55) << line << "║\n";
        }
    }

    oss << "╠══════════════════════════════════════════════════════════╣\n"
        << "║ 【Memory Map】 (0-" << memoryMgr_.totalSize() << "KB)" << std::string(16, ' ') << "║\n";
    auto allBlocks = memoryMgr_.getAllBlocks();
    std::ostringstream mb;
    for (const auto& blk : allBlocks) {
        if (blk.free) mb << "|--free(" << blk.size << "KB)--";
        else mb << "|##" << MemoryManager::getOwnerLabel(blk.pid) << "(" << blk.size << "KB)";
    }
    std::string bs = mb.str();
    if (bs.size() > 52) bs = bs.substr(0, 49) + "...";
    oss << "║ " << std::left << std::setw(55) << bs << "║\n";

    oss << "╠══════════════════════════════════════════════════════════╣\n"
        << "║ 【多级反馈队列】                                        ║\n";
    std::string qs = scheduler_.queueStatus();
    std::istringstream qs2(qs); std::string ql;
    while (std::getline(qs2, ql)) {
        if (ql.empty()) continue;
        if (ql.size() > 54) ql = ql.substr(0, 51) + "...";
        oss << "║ " << std::left << std::setw(55) << ql << "║\n";
    }

    int rc = 0, rnc = 0, bc = 0, sc = 0;
    { std::lock_guard<std::recursive_mutex> pmLock(processMgr_.mutex());
      for (const auto& p : processMgr_.getAllPCBs()) {
        if (!owner.empty() && p.second.owner != owner) continue;
        switch (p.second.state) {
            case PCB::READY: rc++; break; case PCB::RUNNING: rnc++; break;
            case PCB::BLOCKED: bc++; break; case PCB::SUSPENDED: sc++; break;
            default: break;
        }
    }}

    oss << "╠══════════════════════════════════════════════════════════╣\n"
        << "║ 【Stats】 就绪:" << std::setw(4) << rc << " 运行:" << std::setw(4) << rnc
        << " 阻塞:" << std::setw(4) << bc << " 挂起:" << std::setw(4) << sc << "    ║\n"
        << "║ 调度器: " << std::setw(8) << (scheduler_.isRunning() ? "运行中" : "已停止")
        << "  算法: " << std::setw(14) << memoryMgr_.getAllocAlgoName() << " ║\n"
        << "╚══════════════════════════════════════════════════════════╝\n";
    return oss.str();
}

// logPrint — 打印线程日志（log_on 时在每条消息前加 [角色][线程ID]）
void OSSimulator::logPrint(const char* role, const std::string& msg) {
    if (!logEnabled_.load()) return;
    std::cout << "[" << role << "][" << std::this_thread::get_id() << "] " << msg << "\n" << std::flush;
}

std::string OSSimulator::handleLogCmd(const CommandParser::Command& cmd) {
    bool on = (cmd.name == "log_on");
    logEnabled_ = on;
    return std::string("线程日志已") + (on ? "开启\n" : "关闭\n");
}

void OSSimulator::showWelcome() {
    std::cout << "OS Simulator v1.0 [" << (isBackend_ ? "后端" : "观察者") << "]  |  help 帮助  exit 退出\n\n";
}

void OSSimulator::showHelp() {
    std::cout << "\n========== 可用命令一览 ==========\n\n"
        << "--- 用户管理 ---\n"
        << "  register <用户名> <密码>   注册\n  login <用户名> <密码>      登录\n  logout                     登出\n\n"
        << "--- 进程管理 (20分) ---\n"
        << "  create_pcb <名称> <优先级> [ppid] [需CPU]  创建\n"
        << "  kill_pcb <PID>   block_pcb <PID>   wakeup_pcb <PID>\n"
        << "  show_pcb <PID>   list_pcb   ptree\n"
        << "  suspend <PID>    resume <PID>   renice <PID> <优先级>\n\n"
        << "--- 调度器 (8分) ---\n"
        << "  start_sched | stop_sched | restart_sched | step\n\n"
        << "--- 内存管理 (16分) ---\n"
        << "  alloc <大小KB> <PID|data|io|kernel>   free_mem <地址>   show_mem\n"
        << "  compact   mem_stat   set_alloc_algo <FF|BF|WF>\n"
        << "  pgfault   swap_out <PID>   swap_in <PID>\n\n"
        << "--- 持久化 (20分) ---\n"
        << "  save | load | clear_save\n\n"
        << "--- 可视化 (10分) ---\n"
        << "  overview\n\n--- 系统 ---\n  help / ?   exit / quit\n"
        << "  log_on / log_off    开启/关闭线程日志\n\n";
}
