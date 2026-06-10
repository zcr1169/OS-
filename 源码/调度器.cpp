// 多级反馈队列调度器 (MLFQ)
// Q0(时间片2) → Q1(时间片4) → Q2(时间片8)
// 新进程默认入 Q0，时间片耗尽降级，唤醒/恢复重置 Q0
#include "调度器.h"
#include "进程管理器.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

const int Scheduler::TIME_SLICE[3] = {2, 4, 8};

Scheduler::Scheduler()
    : pm_(nullptr), running_(false), stopRequested_(false)
    , interval_(std::chrono::milliseconds(2000)), scheduleCount_(0) {
    queues_.resize(3);
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::init(ProcessManager* pm) {
    std::lock_guard<std::mutex> lock(mtx_);
    pm_ = pm;
    queues_.clear();
    queues_.resize(3);
}

// enqueue — 加入调度队列，priority 决定去哪个队列（0→Q0, 5→Q1, 10→Q2）
void Scheduler::enqueue(int32_t pid, int32_t priority) {
    std::lock_guard<std::mutex> lock(mtx_);
    int qIdx = priorityToQueue(priority);
    for (int i = 0; i < 3; i++) {
        auto& q = queues_[i];
        auto it = std::find(q.begin(), q.end(), pid);
        if (it != q.end()) { q.erase(it); }
    }
    queues_[qIdx].push_back(pid);
}

// dequeue — 从调度队列移除进程（block/suspend/kill 时用）
void Scheduler::dequeue(int32_t pid) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < 3; i++) {
        auto& q = queues_[i];
        auto it = std::find(q.begin(), q.end(), pid);
        if (it != q.end()) { q.erase(it); return; }
    }
}

// start — 启动自动调度，扫描就绪进程全部入 Q0，启动调度线程
void Scheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopRequested_ = false;

    if (pm_) {
        std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
        auto pids = pm_->getSchedulableProcesses();
        for (int32_t pid : pids) {
            if (pid == 1) continue;
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->state == PCB::READY && pcb->priority >= 0) {
                enqueue(pid, 0);
            }
        }
    }

    if (schedThread_.joinable()) schedThread_.join();
    schedThread_ = std::thread(&Scheduler::schedulerLoop, this);
}

// stop — 停止调度，通知线程退出并等待
void Scheduler::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    stopRequested_ = true;
    cv_.notify_all();
    if (schedThread_.joinable()) schedThread_.join();
}

// restart — 重启调度，保留当前队列和调度计数
void Scheduler::restart() {
    bool wasRunning = running_.exchange(false);
    stopRequested_ = true;
    cv_.notify_all();
    if (schedThread_.joinable()) schedThread_.join();

    stopRequested_ = false;
    running_ = true;
    schedThread_ = std::thread(&Scheduler::schedulerLoop, this);
}

// step — 单步调度
// 输出完整决策链路：当前队列 → 选中谁 → 时间片 → CPU时间 → 完成/降级 → 执行后队列
std::string Scheduler::step() {
    if (!pm_) return "调度器未初始化\n";

    std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
    std::lock_guard<std::mutex> schedLock(mtx_);

    std::ostringstream oss;

    // 输出当前队列
    oss << "当前队列状态:\n";
    {
        const char* rangeLabels[] = {"优先级 0-3", "优先级 4-7", "优先级 8-15"};
        for (int i = 0; i < 3; i++) {
            oss << "  Q" << i << "(" << rangeLabels[i] << "): ";
            if (queues_[i].empty()) { oss << "(空)"; }
            else {
                bool first = true;
                for (int32_t qpid : queues_[i]) {
                    if (!first) oss << " -> "; first = false;
                    PCB* qpcb = pm_->getPCB(qpid);
                    if (qpcb) oss << qpcb->name << "(" << qpid << ")["
                                  << qpcb->cpuTime << "/" << qpcb->burstTime << "]";
                    else oss << "?" << qpid;
                }
            }
            oss << "\n";
        }
    }

    for (int qIdx = 0; qIdx < 3; qIdx++) {
        auto& q = queues_[qIdx];
        if (q.empty()) continue;

        // 清理队首无效进程
        bool sawInit = false;
        while (!q.empty()) {
            int32_t frontPid = q.front();
            if (frontPid == 1) {
                q.pop_front();
                if (sawInit) break;
                q.push_back(frontPid);
                sawInit = true;
                continue;
            }
            PCB* pcb = pm_->getPCB(frontPid);
            if (!pcb || pcb->state != PCB::READY) {
                q.pop_front();
            } else { break; }
        }
        if (q.empty() || (q.size() == 1 && q.front() == 1)) continue;

        int32_t pid = q.front(); q.pop_front();
        PCB* pcb = pm_->getPCB(pid);
        int timeSlice = TIME_SLICE[qIdx];
        pcb->state = PCB::RUNNING;
        pcb->cpuTime += timeSlice;

        oss << "[调度] >> 选中 PID=" << pid << " (" << pcb->name
            << ") 来自 Q" << qIdx
            << "(优先级 " << (qIdx <= 1 ? (std::to_string(qIdx*4) + "-" + std::to_string(qIdx*4+3)) : "8-15") << ")"
            << " | 时间片=" << timeSlice
            << " | CPU=" << pcb->cpuTime << "/" << pcb->burstTime << "\n"
            << "   决策: Q" << qIdx << " 非空 → 取 " << pcb->name
            << " → 时间片 " << timeSlice << " | ";

        if (pcb->cpuTime >= pcb->burstTime) {
            pcb->state = PCB::TERMINATED;
            oss << "CPU " << pcb->cpuTime << " >= " << pcb->burstTime << " → 进程完成! 已自动终止\n";
            if (onTerminate_) onTerminate_(pid);
            pcb->memoryBlocks.clear();
            pcb->totalMemory = 0;
            scheduleCount_++;
            if (scheduleCount_ % AGE_INTERVAL == 0) {
                agePriorities();
                oss << "   ⚡ 老化触发: Q2/Q1 队首进程已提升优先级\n";
            }
        } else {
            pcb->state = PCB::READY;
            int nextQ = std::min(qIdx + 1, 2);
            int prioQ = priorityToQueue(pcb->priority);
            int targetQ = std::min(std::max(nextQ, prioQ), 2);
            queues_[targetQ].push_back(pid);

            if (targetQ > qIdx)
                oss << "CPU " << pcb->cpuTime << " < " << pcb->burstTime << " → 时间片耗尽, 降级 Q" << qIdx << "→Q" << targetQ << "\n";
            else
                oss << "CPU " << pcb->cpuTime << " < " << pcb->burstTime << " → 已在最低级 Q" << targetQ << ", 继续排队\n";
            scheduleCount_++;
            if (scheduleCount_ % AGE_INTERVAL == 0) {
                agePriorities();
                oss << "   ⚡ 老化触发: Q2/Q1 队首进程已提升优先级\n";
            }
        }

        oss << "   执行后队列: ";
        {
            bool hasAny = false;
            for (int i = 0; i < 3; i++) {
                if (!queues_[i].empty()) {
                    if (hasAny) oss << " | "; hasAny = true;
                    oss << "Q" << i << "=";
                    bool fst = true;
                    for (int32_t qpid : queues_[i]) {
                        if (!fst) oss << "->"; fst = false;
                        PCB* qpcb = pm_->getPCB(qpid);
                        if (qpcb) oss << qpcb->name << "(" << qpid << ")["
                                      << qpcb->cpuTime << "/" << qpcb->burstTime << "]";
                        else oss << "?" << qpid;
                    }
                }
            }
            if (!hasAny) oss << "(全空)";
        }
        oss << "\n";
        return oss.str();
    }

    // 队列全空，尝试回填就绪进程
    if (pm_) {
        oss << "所有调度队列为空, 尝试回填就绪进程...\n";
        auto pids = pm_->getSchedulableProcesses();
        int filled = 0;
        for (int32_t pid : pids) {
            if (pid == 1) continue;
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->state == PCB::READY) {
                queues_[0].push_back(pid);
                filled++;
            }
        }
        if (filled > 0) {
            oss << "  回填了 " << filled << " 个进程, 请再次执行 step\n";
            return oss.str();
        }
    }
    return "所有调度队列为空, 无可调度进程\n";
}

std::string Scheduler::status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream oss;
    oss << "调度器状态: " << (running_ ? "运行中" : "已停止") << "\n";
    oss << "调度次数: " << scheduleCount_ << "\n";
    oss << "调度间隔: " << interval_.count() << "ms\n";
    return oss.str();
}

// queueStatus — 返回三队列的进程列表（overview 用）
std::string Scheduler::queueStatus() const {
    std::vector<std::vector<int32_t>> queuePids(3);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int i = 0; i < 3; i++)
            queuePids[i].assign(queues_[i].begin(), queues_[i].end());
    }

    std::unordered_map<int32_t, std::pair<std::string, std::string>> pidInfo;
    if (pm_) {
        std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
        const auto& pcbs = pm_->getAllPCBs();
        for (int i = 0; i < 3; i++)
            for (int32_t pid : queuePids[i]) {
                auto it = pcbs.find(pid);
                if (it != pcbs.end())
                    pidInfo[pid] = {it->second.name,
                        std::to_string(it->second.cpuTime) + "/" + std::to_string(it->second.burstTime)};
            }
    }

    std::ostringstream oss;
    const char* rangeLabels[] = {"优先级 0-3", "优先级 4-7", "优先级 8-15"};
    for (int i = 0; i < 3; i++) {
        oss << "Q" << i << "(" << rangeLabels[i] << "): ";
        if (queuePids[i].empty()) oss << "(空)";
        else {
            for (size_t j = 0; j < queuePids[i].size(); j++) {
                if (j > 0) oss << " -> ";
                int32_t pid = queuePids[i][j];
                auto it = pidInfo.find(pid);
                if (it != pidInfo.end())
                    oss << it->second.first << "(" << pid << ")[" << it->second.second << "]";
                else oss << "?" << pid;
            }
        }
        oss << "\n";
    }
    return oss.str();
}

const std::deque<int32_t>& Scheduler::getQueue(int idx) const {
    static std::deque<int32_t> empty;
    if (idx < 0 || idx >= 3) return empty;
    return queues_[idx];
}

void Scheduler::setQueue(int idx, const std::deque<int32_t>& q) {
    if (idx >= 0 && idx < 3) queues_[idx] = q;
}

int Scheduler::getTimeSlice(int queueIdx) const {
    if (queueIdx < 0 || queueIdx >= 3) return 1;
    return TIME_SLICE[queueIdx];
}

int Scheduler::priorityToQueue(int32_t priority) const {
    if (priority <= 3) return 0;
    if (priority <= 7) return 1;
    return 2;
}

// agePriorities — 老化，每10次调度把Q2队首→Q1、Q1队首→Q0，防饥饿
void Scheduler::agePriorities() {
    auto promoteFront = [this](int srcQ, int dstQ, int32_t maxPrio) {
        auto& src = queues_[srcQ];
        if (src.empty()) return;
        int32_t pid = src.front(); src.pop_front();
        queues_[dstQ].push_back(pid);
        if (pm_) {
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->priority > maxPrio) pcb->priority = maxPrio;
        }
    };
    promoteFront(2, 1, 7);
    promoteFront(1, 0, 3);
}

// schedulerLoop — 后台调度线程，每2秒调一次step，连续3次空队列自动停止
void Scheduler::schedulerLoop() {
    int emptyRounds = 0;
    while (!stopRequested_.load()) {
        if (running_.load()) {
            if (logCb_) logCb_("调度", "自动调度触发");
            std::string log = step();
            if (!log.empty() && log.find("所有调度队列为空") != std::string::npos) {
                emptyRounds++;
                if (emptyRounds >= 3) {
                    if (logCb_) logCb_("调度", "所有进程已完成, 调度器自动停止");
                    std::cout << "\n[调度] 所有用户进程已完成, 调度器自动停止\n" << std::flush;
                    running_ = false; break;
                }
            } else if (!log.empty()) {
                std::cout << "\n" << log << std::flush;
                emptyRounds = 0;
            }
        }
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, interval_, [this] { return stopRequested_.load(); });
    }
}
