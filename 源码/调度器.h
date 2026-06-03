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

class ProcessManager;

// 多级反馈队列调度器
// Q0: 优先级0-3, 时间片1
// Q1: 优先级4-7, 时间片2
// Q2: 优先级8-15, 时间片4
// 用完时间片降级, 定期老化防饥饿
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // 绑定进程管理器
    void init(ProcessManager* pm);

    // 将进程加入调度队列
    void enqueue(int32_t pid, int32_t priority);

    // 从调度队列移除进程
    void dequeue(int32_t pid);

    // 启动后台调度线程
    void start();

    // 停止调度
    void stop();

    // 重启调度
    void restart();

    // 单步执行一次调度
    std::string step();

    // 调度器运行状态
    std::string status() const;

    // 各队列的进程列表(overview用)
    std::string queueStatus() const;

    // 是否正在运行
    bool isRunning() const { return running_.load(); }

    // 获取队列(持久化用, 调用者自己加锁)
    const std::deque<int32_t>& getQueue(int idx) const;
    // 设置队列(load恢复用, 调用者自己加锁)
    void setQueue(int idx, const std::deque<int32_t>& q);

    // 获取时间片
    int getTimeSlice(int queueIdx) const;

    // 锁(注意: condition_variable只能用std::mutex)
    std::mutex& mutex() const { return mtx_; }

private:
    // 后台调度线程主循环
    void schedulerLoop();

    // 根据优先级确定队列
    int priorityToQueue(int32_t priority) const;

    // 优先级老化(调用时需已持有pm锁和sched锁)
    void agePriorities();

    ProcessManager* pm_;
    std::vector<std::deque<int32_t>> queues_;
    std::atomic<bool> running_;
    std::atomic<bool> stopRequested_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::thread schedThread_;            // 调度线程(可join)
    std::chrono::milliseconds interval_; // 调度间隔

    static const int TIME_SLICE[3];
    static const int AGE_INTERVAL = 10;
    int scheduleCount_;
};
