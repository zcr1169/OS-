#pragma once
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

/** UTF-8 string 转 UTF-16 wstring (Windows内部需要) */
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

/**
 * 跨平台文件锁
 * 用于多实例场景下确保只有一个后台线程维护状态文件
 * Windows: 使用 CreateFileW + LockFileEx (支持Unicode路径)
 * POSIX:   使用 flock
 */
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

    /** 尝试获取文件锁(非阻塞) */
    bool tryLock(const std::string& filePath) {
#ifdef _WIN32
        std::wstring wpath = utf8ToWide(filePath);
        handle_ = CreateFileW(wpath.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_ALWAYS,
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

    /** 释放文件锁 */
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
