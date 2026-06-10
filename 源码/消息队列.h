// 线程安全消息队列 — 前台→后台的命令传递
#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

template <typename T>
class MessageQueue {
public:
    void push(const T& msg) {
        { std::lock_guard<std::mutex> lock(mtx_); queue_.push(msg); }
        cv_.notify_one();
    }

    bool pop(T& msg, int timeoutMs = -1) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (timeoutMs < 0)
            cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        else
            cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return !queue_.empty() || stopped_; });
        if (stopped_ || queue_.empty()) return false;
        msg = std::move(queue_.front()); queue_.pop();
        return true;
    }

    bool tryPop(T& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;
        msg = std::move(queue_.front()); queue_.pop();
        return true;
    }

    void stop() { { std::lock_guard<std::mutex> lock(mtx_); stopped_ = true; } cv_.notify_all(); }
    bool empty() const { std::lock_guard<std::mutex> lock(mtx_); return queue_.empty(); }

private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;
};
