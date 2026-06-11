// 进程管理器 — 进程创建、销毁、状态管理
#include "进程管理器.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

ProcessManager::ProcessManager() : nextPid_(0) {}

// createPCB — 创建进程
// 分配 PID，初始状态 READY，插入哈希表。如果指定了父进程则加入父进程 children 列表
int32_t ProcessManager::createPCB(const std::string& name, int32_t priority,
                                   int32_t ppid, const std::string& owner,
                                   int32_t burstTime) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (ppid >= 0 && pcbs_.find(ppid) == pcbs_.end()) {
        return -1;
    }
    if (priority < 0) priority = 0;
    if (priority > 15) priority = 15;

    int32_t pid = nextPid_++;
    PCB pcb(pid, ppid, name, PCB::READY, priority, owner, burstTime);
    pcbs_[pid] = pcb;

    if (ppid >= 0) {
        pcbs_[ppid].children.push_back(pid);
    }
    return pid;
}

// killPCB — 撤销进程
// 级联删除：递归杀子进程 → 回调释放调度队列和内存 → 从哈希表删除
bool ProcessManager::killPCB(int32_t pid,
                             std::function<void(int32_t)> onKill) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (pid == 0) return false;

    int32_t ppid = it->second.ppid;
    if (ppid >= 0) {
        auto pit = pcbs_.find(ppid);
        if (pit != pcbs_.end()) {
            auto& children = pit->second.children;
            children.erase(std::remove(children.begin(), children.end(), pid),
                          children.end());
        }
    }

    killChildren(pid, onKill);
    if (onKill) onKill(pid);

    it->second.memoryBlocks.clear();
    it->second.totalMemory = 0;
    it->second.state = PCB::TERMINATED;
    pcbs_.erase(pid);
    return true;
}

void ProcessManager::killChildren(int32_t pid, std::function<void(int32_t)>& onKill) {
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return;

    auto children = it->second.children;
    for (int32_t childPid : children) {
        auto cit = pcbs_.find(childPid);
        if (cit != pcbs_.end()) {
            killChildren(childPid, onKill);
            if (onKill) onKill(childPid);
            cit->second.memoryBlocks.clear();
            cit->second.totalMemory = 0;
            cit->second.state = PCB::TERMINATED;
            pcbs_.erase(childPid);
        }
    }
    it->second.children.clear();
}

bool ProcessManager::blockPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (pid == 0) return false;
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state == PCB::TERMINATED || it->second.state == PCB::SUSPENDED)
        return false;
    it->second.state = PCB::BLOCKED;
    return true;
}

bool ProcessManager::wakeupPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state != PCB::BLOCKED) return false;
    it->second.state = PCB::READY;
    return true;
}

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

// listPCB — 列出用户自己的进程，按 owner 过滤
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
        if (!owner.empty() && p.owner != owner) continue;
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

// pTree — 递归遍历 children 画进程树
std::string ProcessManager::pTree(const std::string& owner) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string out;

    for (const auto& pair : pcbs_) {
        const PCB& p = pair.second;
        if (p.ppid == -1 && (owner.empty() || p.owner == owner)) {
            pTreeRecursive(p.pid, 0, owner, out);
        }
    }
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

// suspend — 挂起进程，只移出调度队列，不释放内存
bool ProcessManager::suspendPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end() || pid == 0) return false;
    if (it->second.state != PCB::READY && it->second.state != PCB::RUNNING)
        return false;
    it->second.state = PCB::SUSPENDED;
    return true;
}

bool ProcessManager::resumePCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    if (it->second.state != PCB::SUSPENDED) return false;
    it->second.state = PCB::READY;
    return true;
}

bool ProcessManager::renice(int32_t pid, int32_t newPriority) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (newPriority < 0 || newPriority > 15) return false;
    auto it = pcbs_.find(pid);
    if (it == pcbs_.end()) return false;
    it->second.priority = newPriority;
    return true;
}

PCB* ProcessManager::getPCB(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    return (it != pcbs_.end()) ? &it->second : nullptr;
}

const PCB* ProcessManager::getPCB(int32_t pid) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = pcbs_.find(pid);
    return (it != pcbs_.end()) ? &it->second : nullptr;
}

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

void ProcessManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    pcbs_.clear();
    nextPid_ = 0;
}
