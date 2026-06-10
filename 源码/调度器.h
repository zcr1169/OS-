// 调度器 — 头文件
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

// MLFQ 调度器
// Q0(优先级0-3,时间片2) Q1(4-7,4) Q2(8-15,8)
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void init(ProcessManager* pm);
    void enqueue(int32_t pid, int32_t priority);
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
    std::vector<std::deque<int32_t>> queues_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread schedThread_;
    std::chrono::milliseconds interval_;
    static const int TIME_SLICE[3];
    static const int AGE_INTERVAL = 10;
    int scheduleCount_;
    std::function<void(int32_t)> onTerminate_;
};
