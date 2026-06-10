// 进程管理器 — 头文件
#pragma once
#include "进程控制块.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <functional>

class ProcessManager {
public:
    ProcessManager();

    int32_t createPCB(const std::string& name, int32_t priority,
                      int32_t ppid, const std::string& owner,
                      int32_t burstTime);
    bool killPCB(int32_t pid, std::function<void(int32_t)> onKill = nullptr);
    bool blockPCB(int32_t pid);
    bool wakeupPCB(int32_t pid);
    std::string showPCB(int32_t pid) const;
    std::string listPCB(const std::string& owner) const;
    std::string pTree(const std::string& owner) const;
    bool suspendPCB(int32_t pid);
    bool resumePCB(int32_t pid);
    bool renice(int32_t pid, int32_t newPriority);

    PCB* getPCB(int32_t pid);
    const PCB* getPCB(int32_t pid) const;
    std::vector<int32_t> getProcessesByUser(const std::string& owner) const;
    std::vector<int32_t> getSchedulableProcesses() const;
    const auto& getAllPCBs() const { return pcbs_; }
    auto& getAllPCBsUnsafe() { return pcbs_; }
    int32_t nextPid() const { return nextPid_; }
    void setNextPid(int32_t pid) { nextPid_ = pid; }
    void clear();
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    void killChildren(int32_t pid, std::function<void(int32_t)>& onKill);
    void pTreeRecursive(int32_t pid, int depth, const std::string& owner,
                        std::string& out) const;

    std::unordered_map<int32_t, PCB> pcbs_;    // PID → PCB 哈希表
    int32_t nextPid_;                          // 下一个可用 PID
    mutable std::recursive_mutex mtx_;
};
