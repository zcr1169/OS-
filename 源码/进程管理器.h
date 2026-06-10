// ============================================================
// 进程管理器 — 头文件声明
// ============================================================
#pragma once
#include "进程控制块.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <functional>

/**
 * ProcessManager — 进程管理器
 *
 * 核心数据结构：unordered_map<int32_t, PCB>
 *   - 键：PID（进程唯一标识）
 *   - 值：PCB（进程控制块，包含进程全部信息）
 *
 * 为什么用 unordered_map 而不是 vector？
 *   - 进程的 PID 不会连续（有删除操作），vector 会有空洞
 *   - unordered_map 的查找、插入、删除都是 O(1)
 *
 * 为什么用 recursive_mutex？
 *   - PCB 操作中有些函数会递归调用自己（比如 killChildren）
 *   - 普通 mutex 在同一个线程重复 lock 会死锁
 *   - recursive_mutex 允许同一个线程多次 lock
 */
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

    std::unordered_map<int32_t, PCB> pcbs_;  // 核心数据结构：PCB 哈希表
    int32_t nextPid_;                        // 下一个可用的 PID（自增分配）
    mutable std::recursive_mutex mtx_;       // 多线程安全锁
};
