// ============================================================
// 进程管理器 — 进程的创建、销毁、状态管理
//
// 维护一个全局的 PCB 哈希表（pid → PCB），
// 提供所有进程相关的命令实现。
//
// 关键设计：
//   - 所有操作加 recursive_mutex 保护（多线程安全）
//   - kill_pcb 支持级联删除子进程（递归杀子孙）
//   - ptree 递归遍历 children 列表画树形结构
//   - 用户隔离：list_pcb / ptree 按 owner 过滤
// ============================================================

#include "进程管理器.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

ProcessManager::ProcessManager() : nextPid_(1) {}

/**
 * createPCB — 创建进程（实现 create_pcb 命令）
 *
 * 流程：
 *   1. 检查父进程是否存在（如果指定了 ppid）
 *   2. 限制优先级在 0-15 范围内
 *   3. 分配一个新 PID（nextPid_ 自增）
 *   4. 构造 PCB 对象，初始状态为 READY
 *   5. 插入哈希表 pcbs_
 *   6. 如果指定了父进程，把自己加入父进程的 children 列表
 *
 * 参数：
 *   name      — 进程名
 *   priority  — 优先级 0-15（超出范围会被截断）
 *   ppid      — 父进程 PID，-1 表示无父
 *   owner     — 所属用户名（用于用户隔离）
 *   burstTime — 需要多少 CPU 时间跑完
 *
 * 返回：新进程的 PID，失败返回 -1（父进程不存在）
 */
int32_t ProcessManager::createPCB(const std::string& name, int32_t priority,
                                   int32_t ppid, const std::string& owner,
                                   int32_t burstTime) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (ppid >= 0 && pcbs_.find(ppid) == pcbs_.end()) {
        return -1;  // 父进程不存在
    }

    if (priority < 0) priority = 0;
    if (priority > 15) priority = 15;

    int32_t pid = nextPid_++;           // PID 自增分配
    PCB pcb(pid, ppid, name, PCB::READY, priority, owner, burstTime);
    pcbs_[pid] = pcb;

    if (ppid >= 0) {
        pcbs_[ppid].children.push_back(pid);  // 加入父进程的子进程列表
    }

    return pid;
}

/**
 * killPCB — 撤销进程（实现 kill_pcb 命令）
 *
 * 这是一个递归操作：先杀子进程，再杀自己。
 * 级联删除确保不会产生"孤儿进程"。
 *
 * onKill 回调的作用：
 *   调用方通过回调来释放调度队列和内存资源：
 *     scheduler_.dequeue(killedPid)
 *     memoryMgr_.freeByPid(killedPid)
 *   这样进程管理器不需要依赖调度器和内存管理器
 *
 * 保护规则：
 *   - pid=1（init）不能被 kill
 */
bool ProcessManager::killPCB(int32_t pid,
                             std::function<void(int32_t)> onKill) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (pid == 1) return false;  // init 不能杀

    // 1) 从父进程的 children 列表中移除自己
    int32_t ppid = it->second.ppid;
    if (ppid >= 0) {
        auto pit = pcbs_.find(ppid);
        if (pit != pcbs_.end()) {
            auto& children = pit->second.children;
            children.erase(std::remove(children.begin(), children.end(), pid),
                          children.end());
        }
    }

    // 2) 递归杀子进程（先杀子，再杀父）
    killChildren(pid, onKill);

    // 3) 回调释放自己的资源
    if (onKill) onKill(pid);

    // 4) 清理自己的内存记录
    it->second.memoryBlocks.clear();
    it->second.totalMemory = 0;

    // 5) 从哈希表删除
    it->second.state = PCB::TERMINATED;
    pcbs_.erase(pid);

    return true;
}

/**
 * killChildren — 递归撤销子进程
 *
 * 这是 killPCB 的辅助函数，递归地杀掉一个进程的所有子孙。
 * 先复制一份 children 列表再遍历，因为递归中会修改原列表。
 */
void ProcessManager::killChildren(int32_t pid, std::function<void(int32_t)>& onKill) {
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return;

    auto children = it->second.children;  // 复制！递归中会修改
    for (int32_t childPid : children) {
        auto cit = pcbs_.find(childPid);
        if (cit != pcbs_.end()) {
            killChildren(childPid, onKill);          // 递归杀孙子
            if (onKill) onKill(childPid);             // 释放调度队列+内存
            cit->second.memoryBlocks.clear();
            cit->second.totalMemory = 0;
            cit->second.state = PCB::TERMINATED;
            pcbs_.erase(childPid);                    // 删除子进程
        }
    }
    it->second.children.clear();
}

/**
 * blockPCB — 阻塞进程（实现 block_pcb 命令）
 *
 * 把进程状态从 READY/RUNNING 改为 BLOCKED，
 * 调用方（系统模拟器）会同时 dequeue 从调度队列移除。
 *
 * 限制：
 *   - 不能阻塞 init（pid=1）
 *   - 已终止或已挂起的进程不能阻塞
 */
bool ProcessManager::blockPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (pid == 1) return false;
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state == PCB::TERMINATED || it->second.state == PCB::SUSPENDED)
        return false;
    it->second.state = PCB::BLOCKED;
    return true;
}

/**
 * wakeupPCB — 唤醒进程（实现 wakeup_pcb 命令）
 *
 * 把进程状态从 BLOCKED 改回 READY，
 * 调用方会 enqueue(pid, 0) 将其重新入队到 Q0。
 *
 * MLFQ 规则：唤醒后重置到 Q0（不是回到原来的优先级队列）
 */
bool ProcessManager::wakeupPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state != PCB::BLOCKED) return false;
    it->second.state = PCB::READY;
    return true;
}

/** showPCB — 查看单个 PCB 详细信息（实现 show_pcb 命令） */
std::string ProcessManager::showPCB(int32_t pid) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return "进程 PID=" + std::to_string(pid) + " 不存在\n";

    const PCB& p = it->second;
    std::ostringstream oss;
    oss << "===== PCB 详细信息 =====\n"
        << "PID:       " << p.pid << "\n"
        << "名称:      " << p.name << "\n"
        << "父进程PID: " << p.ppid << "\n"
        << "状态:      " << PCB::stateToString(p.state) << "\n"
        << "优先级:    " << p.priority << "\n"
        << "CPU时间:   " << p.cpuTime << "/" << p.burstTime << "\n"
        << "占用内存:  " << p.totalMemory << " KB\n"
        << "所属用户:  " << p.owner << "\n"
        << "子进程数:  " << p.children.size() << "\n"
        << "内存块数:  " << p.memoryBlocks.size() << "\n";
    if (!p.memoryBlocks.empty()) {
        oss << "内存块列表:\n";
        for (auto& blk : p.memoryBlocks) {
            oss << "  [起始=" << blk.first << "KB, 大小=" << blk.second << "KB]\n";
        }
    }
    return oss.str();
}

/**
 * listPCB — 列出所有进程（实现 list_pcb 命令）
 *
 * 按 owner 过滤：只显示当前用户的进程，看不到别人的。
 * 用 padRight 做中文对齐，不会因为中文字符宽度问题导致表格错乱。
 */
std::string ProcessManager::listPCB(const std::string& owner) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::ostringstream oss;
    oss << PCB::padRight("PID", 6)
        << PCB::padRight("名称", 12)
        << PCB::padRight("状态", 8)
        << PCB::padRight("优先级", 7)
        << PCB::padRight("内存(KB)", 9)
        << PCB::padRight("所属用户", 12)
        << "子进程\n";
    oss << std::string(66, '-') << "\n";

    for (const auto& pair : pcbs_) {
        const PCB& p = pair.second;
        if (!owner.empty() && p.owner != owner) continue;  // 用户隔离
        oss << PCB::padRight(std::to_string(p.pid), 6)
            << PCB::padRight(p.name, 12)
            << PCB::padRight(PCB::stateToString(p.state), 8)
            << PCB::padRight(std::to_string(p.priority), 7)
            << PCB::padRight(std::to_string(p.totalMemory), 9)
            << PCB::padRight(p.owner, 12)
            << p.children.size() << "\n";
    }
    return oss.str();
}

/**
 * pTree — 树形显示进程关系（实现 ptree 命令）
 *
 * 算法：
 *   1. 先找"根进程"（ppid == -1 的进程，即 init）
 *   2. 从根进程开始递归遍历 children
 *   3. 对于父进程被过滤掉的子进程，作为独立树根显示
 *
 * 输出格式：
 *   proc_a(2) [就绪, 优先级=3, 内存=0KB]
 *   ├─ child_a(5) [就绪, 优先级=5, 内存=0KB]
 *   ├─ child_b(6) [就绪, 优先级=6, 内存=0KB]
 */
std::string ProcessManager::pTree(const std::string& owner) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;

    // 第一遍：找根进程（无父进程的）
    for (const auto& pair : pcbs_) {
        const PCB& p = pair.second;
        if (p.ppid == -1 && (owner.empty() || p.owner == owner)) {
            pTreeRecursive(p.pid, 0, owner, out);
        }
    }
    // 第二遍：处理父进程被过滤掉的子进程（孤儿进程作为独立树根）
    for (const auto& pair : pcbs_) {
        const PCB& p = pair.second;
        if (p.ppid == -1) continue;
        if (!owner.empty() && p.owner != owner) continue;
        auto parentIt = pcbs_.find(p.ppid);
        if ((parentIt == pcbs_.end()) ||
            (!owner.empty() && parentIt->second.owner != owner)) {
            pTreeRecursive(p.pid, 0, owner, out);
        }
    }
    return out;
}

/** pTreeRecursive — 递归遍历子进程画树 */
void ProcessManager::pTreeRecursive(int32_t pid, int depth,
                                     const std::string& owner,
                                     std::string& out) const {
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return;
    const PCB& p = it->second;
    if (!owner.empty() && p.owner != owner) return;

    for (int i = 0; i < depth; i++) {
        out += (i == depth - 1) ? "├─ " : "   ";
    }
    out += p.name + "(" + std::to_string(p.pid) + ")";
    out += " [" + std::string(PCB::stateToString(p.state))
           + ", 优先级=" + std::to_string(p.priority)
           + ", 内存=" + std::to_string(p.totalMemory) + "KB]";
    out += "\n";

    for (size_t i = 0; i < p.children.size(); i++) {
        pTreeRecursive(p.children[i], depth + 1, owner, out);
    }
}

/**
 * suspendPCB — 挂起进程（实现 suspend 命令）
 *
 * 挂起 ≠ 终止！
 * 挂起只是把进程从调度队列移出，暂停它的执行，
 * 但保留全部状态（包括内存），等 resume 后继续。
 *
 * 限制：只能挂起 READY 或 RUNNING 状态的进程
 */
bool ProcessManager::suspendPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end() || pid == 1) return false;
    if (it->second.state != PCB::READY && it->second.state != PCB::RUNNING)
        return false;
    it->second.state = PCB::SUSPENDED;
    return true;
}

/** resumePCB — 恢复挂起的进程（实现 resume 命令） */
bool ProcessManager::resumePCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state != PCB::SUSPENDED) return false;
    it->second.state = PCB::READY;
    return true;
}

/** renice — 修改进程优先级（实现 renice 命令） */
bool ProcessManager::renice(int32_t pid, int32_t newPriority) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (newPriority < 0 || newPriority > 15) return false;
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    it->second.priority = newPriority;
    return true;
}

/** getPCB — 通过 PID 获取 PCB 指针（非 const 版本） */
PCB* ProcessManager::getPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    return (it != pcbs_.end()) ? &it->second : nullptr;
}

/** getPCB — const 版本 */
const PCB* ProcessManager::getPCB(int32_t pid) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    return (it != pcbs_.end()) ? &it->second : nullptr;
}

/** getProcessesByUser — 获取某用户的所有进程 PID 列表 */
std::vector<int32_t> ProcessManager::getProcessesByUser(const std::string& owner) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::vector<int32_t> result;
    for (const auto& pair : pcbs_) {
        if (pair.second.owner == owner) {
            result.push_back(pair.first);
        }
    }
    return result;
}

/** getSchedulableProcesses — 获取所有可调度的进程（READY 或 RUNNING） */
std::vector<int32_t> ProcessManager::getSchedulableProcesses() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::vector<int32_t> result;
    for (const auto& pair : pcbs_) {
        if (pair.second.state == PCB::READY || pair.second.state == PCB::RUNNING) {
            result.push_back(pair.first);
        }
    }
    return result;
}

/** clear — 清空所有 PCB（clear_save 命令用） */
void ProcessManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    pcbs_.clear();
    nextPid_ = 1;
}
