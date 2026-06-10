// ============================================================
// 跨平台文件锁
//
// 用于多实例场景下区分"后端"和"观察者"。
// 先启动的实例获得文件锁成为后端，后启动的成为观察者。
//
// Windows 实现：
//   用 CreateFileW + LockFileEx 实现。
//   CreateFileW 支持 UTF-8 路径（转 UTF-16）。
//   LockFileEx 加独占锁（EXCLUSIVE），非阻塞（FAIL_IMMEDIATELY）。
//
// POSIX 实现：
//   用 open + flock 实现。
//   flock LOCK_EX（独占锁）+ LOCK_NB（非阻塞）。
//
// 为什么用文件锁而不是其他 IPC 机制？
//   1. 跨进程：Mutex 不能跨进程
//   2. 自动释放：进程退出后 OS 自动解锁，不会残留
//   3. 简单可靠：只需要一个空文件
// ============================================================
#pragma once
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

/** UTF-8 → UTF-16 转换（Windows API 需要） */
inline std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], len);
    return result;
}
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

class FileLock {
public:
    FileLock() : owned_(false) {
#ifdef _WIN32
        handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = -1;
#endif
    }
    ~FileLock() { unlock(); }

    /**
     * tryLock — 尝试获取文件锁（非阻塞）
     *
     * 打开锁文件 → 尝试加独占锁
     * 成功 → owned_ = true，返回 true
     * 失败（锁被别人占着）→ 关闭文件，返回 false
     */
    bool tryLock(const std::string& filePath) {
#ifdef _WIN32
        std::wstring wpath = utf8ToWide(filePath);
        handle_ = CreateFileW(wpath.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ,          // 允许别人读
                              nullptr,
                              OPEN_ALWAYS,              // 不存在就创建
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;

        OVERLAPPED ov = {0};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0, 1, 0, &ov)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        owned_ = true;
        return true;
#else
        fd_ = open(filePath.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) return false;
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        owned_ = true;
        return true;
#endif
    }

    /** unlock — 释放文件锁 */
    void unlock() {
        if (!owned_) return;
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov = {0};
            UnlockFileEx(handle_, 0, 1, 0, &ov);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
            fd_ = -1;
        }
#endif
        owned_ = false;
    }

    bool isLocked() const { return owned_; }

private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int fd_ = -1;
#endif
    bool owned_;
};
