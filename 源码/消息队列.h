// ============================================================
// 线程安全消息队列
//
// 用于前台线程和后台线程之间的命令传递和结果返回。
// 基于 std::mutex + std::condition_variable 实现，
// 支持带超时的 pop 操作。
//
// 工作原理：
//   生产者（前台线程）：push() → 加锁入队 → notify_one 通知消费者
//   消费者（后台线程）：pop()  → 等待条件变量 → 取队首出队
//
// 为什么用 condition_variable 而不是 busy-waiting？
//   如果后台线程用 while(empty) sleep() 轮询，
//   会有延迟且浪费 CPU。condition_variable 让消费者
//   在队列为空时睡眠，生产者 push 时唤醒它。
// ============================================================
#pragma once
#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

template <typename T>
class MessageQueue {
public:
    /** push — 生产者入队，通知消费者 */
    void push(const T& msg) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(msg);
        }
        cv_.notify_one();
    }

    void push(T&& msg) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(std::move(msg));
        }
        cv_.notify_one();
    }

    /**
     * pop — 消费者出队（带超时）
     *
     * timeoutMs < 0：无限等待
     * timeoutMs ≥ 0：等待指定毫秒，超时返回 false
     *
     * 用 condition_variable::wait_for 实现超时，
     * 避免永久阻塞。
     */
    bool pop(T& msg, int timeoutMs = -1) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (timeoutMs < 0) {
            cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        } else {
            cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return !queue_.empty() || stopped_; });
        }
        if (stopped_ && queue_.empty()) return false;
        if (queue_.empty()) return false;
        msg = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    /** tryPop — 非阻塞尝试出队 */
    bool tryPop(T& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;
        msg = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    /** stop — 停止队列，唤醒所有等待的消费者 */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;
};
