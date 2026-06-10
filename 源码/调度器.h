// ============================================================
// 调度器 — 头文件声明
// ============================================================
#pragma once
#include "进程控制块.h"
#include <vector>
#include <deque>
#include <mutex>
#include <string>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <functional>

class ProcessManager;

/**
 * Scheduler — 多级反馈队列调度器
 *
 * Q0: 优先级 0-3, 时间片 2
 * Q1: 优先级 4-7, 时间片 4
 * Q2: 优先级 8-15, 时间片 8
 *
 * 新进程默认入 Q0，时间片耗尽降级，唤醒/恢复重置 Q0。
 * 每 10 次调度老化一次，防止饥饿。
 */
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void init(ProcessManager* pm);
    void enqueue(int32_t pid, int32_t priority);  // priority 决定进哪个队列
    void dequeue(int32_t pid);

    void start();
    void stop();
    void restart();
    std::string step();

    std::string status() const;
    std::string queueStatus() const;

    bool isRunning() const { return running_.load(); }

    const std::deque<int32_t>& getQueue(int idx) const;
    void setQueue(int idx, const std::deque<int32_t>& q);
    int getTimeSlice(int queueIdx) const;

    std::mutex& mutex() const { return mtx_; }
    void setOnTerminate(std::function<void(int32_t)> cb) { onTerminate_ = cb; }

private:
    void schedulerLoop();
    int priorityToQueue(int32_t priority) const;
    void agePriorities();

    ProcessManager* pm_;
    std::vector<std::deque<int32_t>> queues_;  // 三个双端队列，存 PID

    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::thread schedThread_;
    std::chrono::milliseconds interval_;     // 调度间隔（默认 2000ms）

    static const int TIME_SLICE[3];          // {2, 4, 8}
    static const int AGE_INTERVAL = 10;      // 每 10 次调度触发老化
    int scheduleCount_;

    std::function<void(int32_t)> onTerminate_;  // 进程终止回调（释放内存）
};
