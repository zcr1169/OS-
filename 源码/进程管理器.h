#pragma once
#include "进程控制块.h"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>
#include <functional>

// 进程管理器 - 进程的增删改查 + 进程树维护
class ProcessManager {
public:
    ProcessManager();

    // 创建进程, 返回PID, burstTime默认5
    int32_t createPCB(const std::string& name, int32_t priority,
                      int32_t ppid, const std::string& owner,
                      int32_t burstTime = 5);

    // 撤销进程(连带子进程一并撤销), onKill在删除每个进程前回调
    bool killPCB(int32_t pid,
                 std::function<void(int32_t)> onKill = nullptr);

    // 阻塞进程
    bool blockPCB(int32_t pid);

    // 唤醒进程
    bool wakeupPCB(int32_t pid);

    // 查看进程详细信息
    std::string showPCB(int32_t pid) const;

    // 列出所有进程
    std::string listPCB(const std::string& owner = "") const;

    // 树形显示进程父子关系
    std::string pTree(const std::string& owner = "") const;

    // 挂起进程(移出调度队列)
    bool suspendPCB(int32_t pid);

    // 恢复挂起进程
    bool resumePCB(int32_t pid);

    // 修改进程优先级
    bool renice(int32_t pid, int32_t newPriority);

    // 获取某个PCB的指针(内部加了锁, 别长期持有)
    PCB* getPCB(int32_t pid);
    const PCB* getPCB(int32_t pid) const;

    // 获取全部PCB
    const std::unordered_map<int32_t, PCB>& getAllPCBs() const { return pcbs_; }

    // 获取全部PCB(可写, load恢复用)
    std::unordered_map<int32_t, PCB>& getAllPCBsUnsafe() { return pcbs_; }

    // 获取某用户的所有进程
    std::vector<int32_t> getProcessesByUser(const std::string& owner) const;

    // 获取可调度进程列表(就绪/运行态)
    std::vector<int32_t> getSchedulableProcesses() const;

    // 下一个可用的PID
    int32_t nextPid() const { return nextPid_; }

    // 设置下一个PID(load恢复用)
    void setNextPid(int32_t pid) { nextPid_ = pid; }

    // 清空所有进程
    void clear();

    // 锁(外部调度/后台线程用)
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    // 递归撤销子进程
    void killChildren(int32_t pid, std::function<void(int32_t)>& onKill);

    // 递归打印进程树
    void pTreeRecursive(int32_t pid, int depth, const std::string& owner,
                        std::string& out) const;

    std::unordered_map<int32_t, PCB> pcbs_;
    int32_t nextPid_;
    mutable std::recursive_mutex mtx_;
};
