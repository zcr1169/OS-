#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <vector>
#include <mutex>

#ifdef _WIN32
#include <windows.h>

// === 管道I/O辅助 (长度前缀帧协议) ===
namespace {

/** 从管道读指定字节数, 处理短读 */
inline bool pipeReadExact(HANDLE pipe, void* buf, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD n = 0;
        if (!ReadFile(pipe, static_cast<char*>(buf) + total, size - total, &n, nullptr))
            return false;
        if (n == 0) return false;  // 管道对端关闭
        total += n;
    }
    return true;
}

/** 向管道精确写入size字节 */
inline bool pipeWriteExact(HANDLE pipe, const void* buf, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD n = 0;
        if (!WriteFile(pipe, static_cast<const char*>(buf) + total, size - total, &n, nullptr))
            return false;
        total += n;
    }
    return true;
}

/** 读取一条长度前缀消息 */
inline bool pipeReadMsg(HANDLE pipe, std::string& msg) {
    uint32_t len = 0;
    if (!pipeReadExact(pipe, &len, sizeof(len))) return false;
    if (len == 0 || len > 65536) return false;  // 最大64KB
    msg.resize(len);
    return pipeReadExact(pipe, &msg[0], static_cast<DWORD>(len));
}

/** 写入一条长度前缀消息 */
inline bool pipeWriteMsg(HANDLE pipe, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    if (!pipeWriteExact(pipe, &len, sizeof(len))) return false;
    return pipeWriteExact(pipe, msg.data(), static_cast<DWORD>(len));
}

}  // namespace


/**
 * 命名管道服务端(后端实例)
 * 监听客户端连接, 收到命令字符串交给handler执行, 结果通过管道返回。
 */
class PipeServer {
public:
    using CommandHandler = std::function<std::string(const std::string&)>;

    PipeServer() : running_(false) {}
    ~PipeServer() { stop(); }

    /** 启动管道服务, handler在客户端线程中同步调用 */
    bool start(const std::wstring& pipeName, CommandHandler handler) {
        pipeName_ = pipeName;
        running_ = true;
        serverThread_ = std::thread(&PipeServer::serverLoop, this, pipeName, handler);
        return true;
    }

    /** 停止服务, 等待所有客户端线程退出后join */
    void stop() {
        if (!running_.exchange(false)) return;  // 已停止

        // 唤醒阻塞在ConnectNamedPipe的serverLoop线程
        if (!pipeName_.empty()) {
            HANDLE h = CreateFileW(pipeName_.c_str(),
                                   GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }

        // 等所有客户端线程退出
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            for (auto& t : clientThreads_) {
                if (t.joinable()) t.join();
            }
            clientThreads_.clear();
        }

        if (serverThread_.joinable()) serverThread_.join();
    }

    bool isRunning() const { return running_.load(); }

private:
    void serverLoop(const std::wstring& pipeName, CommandHandler handler) {
        while (running_.load()) {
            HANDLE pipe = CreateNamedPipeW(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                65536, 65536,   // 输入/输出缓冲区64KB
                0, nullptr);    // 默认超时

            if (pipe == INVALID_HANDLE_VALUE) {
                if (running_.load()) Sleep(100);  // 短暂等待后重试
                continue;
            }

            BOOL ok = ConnectNamedPipe(pipe, nullptr);
            if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                continue;
            }

            // 每个客户端一个线程
            std::thread t(&PipeServer::handleClient, this, pipe, handler);
            {
                std::lock_guard<std::mutex> lock(clientThreadsMutex_);
                clientThreads_.push_back(std::move(t));
            }
        }
    }

    void handleClient(HANDLE pipe, CommandHandler handler) {
        while (running_.load()) {
            std::string cmd;
            if (!pipeReadMsg(pipe, cmd)) break;  // 客户端断开或出错

            if (cmd.empty()) continue;

            std::string result = handler(cmd);

            if (!pipeWriteMsg(pipe, result)) break;  // 客户端断开
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    std::atomic<bool> running_;
    std::thread serverThread_;
    std::wstring pipeName_;
    std::vector<std::thread> clientThreads_;      // 客户端处理线程列表
    std::mutex clientThreadsMutex_;               // 保护clientThreads_
};


/**
 * 命名管道客户端(观察者实例)
 * 连接后端PipeServer, 发送命令并接收结果。
 */
class PipeClient {
public:
    PipeClient() : pipeHandle_(INVALID_HANDLE_VALUE) {}
    ~PipeClient() { disconnect(); }

    // 连接到命名管道, 超时返回false
    bool connect(const std::wstring& pipeName, int timeoutMs = 3000) {
        disconnect();

        DWORD start = GetTickCount();
        while (true) {
            pipeHandle_ = CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);

            if (pipeHandle_ != INVALID_HANDLE_VALUE) {
                // 设为字节读取模式
                DWORD mode = PIPE_READMODE_BYTE;
                SetNamedPipeHandleState(pipeHandle_, &mode, nullptr, nullptr);
                return true;
            }

            DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
                return false;  // 不可恢复的错误

            if (GetTickCount() - start > static_cast<DWORD>(timeoutMs))
                return false;  // 超时

            // ERROR_PIPE_BUSY: 等待管道实例可用
            // ERROR_FILE_NOT_FOUND: 管道还未创建，等待后重试
            if (err == ERROR_PIPE_BUSY) {
                if (!WaitNamedPipeW(pipeName.c_str(), 200)) {
                    Sleep(200);
                }
            } else {
                Sleep(200);
            }
        }
    }

    void disconnect() {
        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipeHandle_);
            pipeHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool isConnected() const { return pipeHandle_ != INVALID_HANDLE_VALUE; }

    /** 发送命令, 阻塞等待结果. 连接断开时返回错误提示 */
    std::string sendCommand(const std::string& cmd) {
        if (pipeHandle_ == INVALID_HANDLE_VALUE)
            return "[错误] 未连接到后端实例\n";

        if (!pipeWriteMsg(pipeHandle_, cmd)) {
            disconnect();
            return "[错误] 与后端实例的连接已断开\n";
        }

        std::string result;
        if (!pipeReadMsg(pipeHandle_, result)) {
            disconnect();
            return "[错误] 接收后端响应失败, 连接已断开\n";
        }

        return result;
    }

private:
    HANDLE pipeHandle_;
};

#endif  // _WIN32
