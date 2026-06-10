// ============================================================
// 多级反馈队列调度器 (MLFQ — Multi-Level Feedback Queue)
//
// 这是整个课程设计的核心模块，也是老师验收的重点。
//
// 原理：
//   维护 3 个调度队列 Q0、Q1、Q2，优先级从高到低。
//   新进程全部入 Q0，时间片耗尽则降级到下一队列。
//   高优先级队列为空时才调度低优先级队列。
//
// 时间片：Q0=2、Q1=4、Q2=8（2→4→8 倍增）
//
// 验收常考问题：
//   Q: 为什么要设计多级反馈队列？
//   A: 兼顾响应时间和吞吐量。短进程在 Q0 快速完成，
//      长进程降级到 Q2 获得长时间片减少切换开销。
//
//   Q: 什么是"饥饿"？如何解决？
//   A: Q2 的进程可能永远得不到 CPU。
//      解决方法：老化（aging），每 10 次调度从低队列
//      提升队首进程到高队列。
// ============================================================

#include "调度器.h"
#include "进程管理器.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

/** 三级队列的时间片：Q0=2, Q1=4, Q2=8（PPT要求的2→4→8倍增） */
const int Scheduler::TIME_SLICE[3] = {2, 4, 8};

Scheduler::Scheduler()
    : pm_(nullptr)
    , running_(false)
    , stopRequested_(false)
    , interval_(std::chrono::milliseconds(2000))  // 自动调度间隔 2 秒
    , scheduleCount_(0)
{
    queues_.resize(3);  // 初始化三个队列
}

Scheduler::~Scheduler() {
    stop();  // 析构时确保调度线程退出
}

/** init — 绑定进程管理器，清空队列 */
void Scheduler::init(ProcessManager* pm) {
    std::lock_guard<std::mutex> lock(mtx_);
    pm_ = pm;
    queues_.clear();
    queues_.resize(3);
}

/**
 * enqueue — 将进程加入调度队列
 *
 * 根据 priority 值决定进哪个队列：
 *   传 0  → 进 Q0（新进程、唤醒、恢复）
 *   传 5  → 进 Q1
 *   传 10 → 进 Q2
 *
 * 先检查三个队列中是否已有该 PID，有则移除（防止重复入队）
 */
void Scheduler::enqueue(int32_t pid, int32_t priority) {
    std::lock_guard<std::mutex> lock(mtx_);
    int qIdx = priorityToQueue(priority);
    for (int i = 0; i < 3; i++) {
        auto& q = queues_[i];
        auto it = std::find(q.begin(), q.end(), pid);
        if (it != q.end()) {
            q.erase(it);
        }
    }
    queues_[qIdx].push_back(pid);
}

/**
 * dequeue — 从调度队列移除进程
 *
 * 用在 block、suspend、kill 时把进程从队列中移除
 */
void Scheduler::dequeue(int32_t pid) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < 3; i++) {
        auto& q = queues_[i];
        auto it = std::find(q.begin(), q.end(), pid);
        if (it != q.end()) {
            q.erase(it);
            return;
        }
    }
}

/**
 * start — 启动调度器（实现 start_sched 命令）
 *
 * 流程：
 *   1. CAS 原子操作，防止重复启动
 *   2. 扫描所有就绪进程，统一入 Q0（MLFQ 规则）
 *   3. 启动 schedulerLoop 线程（每 2 秒调度一次）
 */
void Scheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    stopRequested_ = false;

    if (pm_) {
        std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
        auto pids = pm_->getSchedulableProcesses();
        for (int32_t pid : pids) {
            if (pid == 1) continue;  // init 不参与调度
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->state == PCB::READY && pcb->priority >= 0) {
                enqueue(pid, 0);  // 新进程默认入 Q0
            }
        }
    }

    if (schedThread_.joinable()) {
        schedThread_.join();
    }

    schedThread_ = std::thread(&Scheduler::schedulerLoop, this);
}

/**
 * stop — 停止调度器（实现 stop_sched 命令）
 *
 * 用 CAS 原子操作安全停止，通过 condition_variable
 * 立即唤醒调度线程检查标志退出。
 */
void Scheduler::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    stopRequested_ = true;
    cv_.notify_all();

    if (schedThread_.joinable()) {
        schedThread_.join();
    }
}

/**
 * restart — 重启调度器（实现 restart_sched 命令）
 *
 * 和 start 的区别：保留当前队列内容和调度计数，
 * 从暂停的地方继续，而不是重新扫描所有进程。
 */
void Scheduler::restart() {
    bool wasRunning = running_.exchange(false);
    stopRequested_ = true;
    cv_.notify_all();
    if (schedThread_.joinable()) schedThread_.join();

    stopRequested_ = false;
    running_ = true;
    schedThread_ = std::thread(&Scheduler::schedulerLoop, this);
}

/**
 * step — 单步执行一次调度（实现 step 命令）
 *
 * 这是调度器的核心函数，PPT 要求输出"完整决策链路"：
 *   1. 当前队列状态 → 2. 选中了谁 → 3. 为什么选它
 *   → 4. 时间片多少 → 5. CPU 时间变化 → 6. 完成/降级
 *   → 7. 执行后队列状态
 *
 * 算法：
 *   按 Q0→Q1→Q2 顺序扫描，取第一个非空队列的队首进程。
 *   时间片耗尽但没跑完 → 降级到下一队列
 *   CPU 时间跑够（cpuTime ≥ burstTime）→ 自动终止 + 释放内存
 */
std::string Scheduler::step() {
    if (!pm_) return "调度器未初始化\n";

    // 统一锁顺序：PM锁 → Sched锁（防止死锁）
    std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
    std::lock_guard<std::mutex> schedLock(mtx_);

    std::ostringstream oss;

    // ======== 第一步：输出当前队列快照 ========
    oss << "当前队列状态:\n";
    {
        const char* rangeLabels[] = {"优先级 0-3", "优先级 4-7", "优先级 8-15"};
        for (int i = 0; i < 3; i++) {
            oss << "  Q" << i << "(" << rangeLabels[i] << "): ";
            if (queues_[i].empty()) {
                oss << "(空)";
            } else {
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

    // ======== 第二步：扫描队列找第一个可调度进程 ========
    for (int qIdx = 0; qIdx < 3; qIdx++) {
        auto& q = queues_[qIdx];
        if (q.empty()) continue;

        // 清理队列头部无效的 PID（已被杀但队列残留的）
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
                oss << "队列" << qIdx << "中的PID " << frontPid << " 无效，已清理\n";
            } else {
                break;
            }
        }
        if (q.empty() || (q.size() == 1 && q.front() == 1)) continue;

        // ======== 第三步：取队首进程执行 ========
        int32_t pid = q.front();
        q.pop_front();
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

        // ======== 第四步：判断完成还是降级 ========
        if (pcb->cpuTime >= pcb->burstTime) {
            // 情况 A：进程执行完毕，自动终止
            pcb->state = PCB::TERMINATED;
            oss << "CPU " << pcb->cpuTime << " >= " << pcb->burstTime << " → 进程完成! 已自动终止\n";
            if (onTerminate_) onTerminate_(pid);  // 回调释放内存
            pcb->memoryBlocks.clear();
            pcb->totalMemory = 0;
            scheduleCount_++;
            if (scheduleCount_ % AGE_INTERVAL == 0) {
                agePriorities();
                oss << "   ⚡ 老化触发: Q2/Q1 队首进程已提升优先级\n";
            }
        } else {
            // 情况 B：时间片用完但没完成，降级到下一队列
            pcb->state = PCB::READY;
            int nextQ = std::min(qIdx + 1, 2);
            int prioQ = priorityToQueue(pcb->priority);
            int targetQ = std::max(nextQ, prioQ);
            targetQ = std::min(targetQ, 2);
            queues_[targetQ].push_back(pid);

            if (targetQ > qIdx) {
                oss << "CPU " << pcb->cpuTime << " < " << pcb->burstTime << " → 时间片耗尽, 降级 Q" << qIdx << "→Q" << targetQ << "\n";
            } else {
                oss << "CPU " << pcb->cpuTime << " < " << pcb->burstTime << " → 已在最低级 Q" << targetQ << ", 继续排队\n";
            }

            scheduleCount_++;

            if (scheduleCount_ % AGE_INTERVAL == 0) {
                agePriorities();
                oss << "   ⚡ 老化触发: Q2/Q1 队首进程已提升优先级\n";
            }
        }

        // ======== 第五步：输出执行后的队列状态 ========
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

    // ======== 队列全空：尝试从进程表回填就绪进程 ========
    if (pm_) {
        oss << "所有调度队列为空, 尝试回填就绪进程...\n";
        auto pids = pm_->getSchedulableProcesses();
        int filled = 0;
        for (int32_t pid : pids) {
            if (pid == 1) continue;
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->state == PCB::READY) {
                queues_[0].push_back(pid);  // 回填到 Q0
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

/** status — 调度器状态信息 */
std::string Scheduler::status() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream oss;
    oss << "调度器状态: " << (running_ ? "运行中" : "已停止") << "\n";
    oss << "调度次数: " << scheduleCount_ << "\n";
    oss << "调度间隔: " << interval_.count() << "ms\n";
    return oss.str();
}

/**
 * queueStatus — 返回各队列的进程列表（overview 用）
 *
 * 格式：Q0(优先级 0-3): A(2)[0/10] -> B(3)[2/10]
 *
 * 分段加锁避免和 step() 互相等锁：
 *   阶段1：拿 sched 锁，拷贝队列快照
 *   阶段2：拿 PM 锁，查进程名称和 CPU 时间
 *   阶段3：格式化输出
 */
std::string Scheduler::queueStatus() const {
    std::vector<std::vector<int32_t>> queuePids(3);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int i = 0; i < 3; i++) {
            queuePids[i].assign(queues_[i].begin(), queues_[i].end());
        }
    }

    std::unordered_map<int32_t, std::pair<std::string, std::string>> pidInfo;
    if (pm_) {
        std::lock_guard<std::recursive_mutex> pmLock(pm_->mutex());
        const auto& pcbs = pm_->getAllPCBs();
        for (int i = 0; i < 3; i++) {
            for (int32_t pid : queuePids[i]) {
                auto it = pcbs.find(pid);
                if (it != pcbs.end()) {
                    pidInfo[pid] = {it->second.name,
                        std::to_string(it->second.cpuTime) + "/" + std::to_string(it->second.burstTime)};
                }
            }
        }
    }

    std::ostringstream oss;
    const char* rangeLabels[] = {"优先级 0-3", "优先级 4-7", "优先级 8-15"};
    for (int i = 0; i < 3; i++) {
        oss << "Q" << i << "(" << rangeLabels[i] << "): ";
        if (queuePids[i].empty()) {
            oss << "(空)";
        } else {
            for (size_t j = 0; j < queuePids[i].size(); j++) {
                if (j > 0) oss << " -> ";
                int32_t pid = queuePids[i][j];
                auto it = pidInfo.find(pid);
                if (it != pidInfo.end()) {
                    oss << it->second.first << "(" << pid << ")[" << it->second.second << "]";
                } else {
                    oss << "?" << pid;
                }
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
    if (idx >= 0 && idx < 3) {
        queues_[idx] = q;
    }
}

int Scheduler::getTimeSlice(int queueIdx) const {
    if (queueIdx < 0 || queueIdx >= 3) return 1;
    return TIME_SLICE[queueIdx];
}

/** priorityToQueue — 根据优先级确定应该进哪个队列 */
int Scheduler::priorityToQueue(int32_t priority) const {
    if (priority <= 3)  return 0;  // Q0
    if (priority <= 7)  return 1;  // Q1
    return 2;                       // Q2
}

/**
 * agePriorities — 老化机制，防止低优先级队列饥饿
 *
 * 每 AGE_INTERVAL（10）次调度触发一次：
 *   Q2 队首 → 提升到 Q1，优先级改为 7
 *   Q1 队首 → 提升到 Q0，优先级改为 3
 *
 * 经典 OS 教材中的"aging"技术。
 */
void Scheduler::agePriorities() {
    auto promoteFront = [this](int srcQ, int dstQ, int32_t maxPrio) {
        auto& src = queues_[srcQ];
        if (src.empty()) return;
        int32_t pid = src.front();
        src.pop_front();
        queues_[dstQ].push_back(pid);
        if (pm_) {
            PCB* pcb = pm_->getPCB(pid);
            if (pcb && pcb->priority > maxPrio) {
                pcb->priority = maxPrio;
            }
        }
    };
    promoteFront(2, 1, 7);
    promoteFront(1, 0, 3);
}

/**
 * schedulerLoop — 后台调度线程主循环
 *
 * 独立于前台和后台线程运行，每 2 秒调用一次 step()。
 * 连续 3 次发现队列全空则自动停止（不会空转浪费 CPU）。
 *
 * 用 condition_variable 实现定时等待，stop_sched 时
 * 可以立即唤醒退出，不用等完 2 秒。
 */
void Scheduler::schedulerLoop() {
    int emptyRounds = 0;
    while (!stopRequested_.load()) {
        if (running_.load()) {
            std::string log = step();
            if (!log.empty() && log.find("所有调度队列为空") != std::string::npos) {
                emptyRounds++;
                if (emptyRounds >= 3) {
                    std::cout << "\n[调度] 所有用户进程已完成, 调度器自动停止\n" << std::flush;
                    running_ = false;
                    break;
                }
            } else if (!log.empty()) {
                std::cout << "\n" << log << std::flush;
                emptyRounds = 0;
            }
        }

        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, interval_, [this] {
            return stopRequested_.load();
        });
    }
}
