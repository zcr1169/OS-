// 跨平台文件锁 — 用于多实例场景区分后端/观察者
#pragma once
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

inline std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring r(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &r[0], len);
    return r;
}
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

// 先启动进程获取文件锁成为后端，后启动进程成为观察者
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

    bool tryLock(const std::string& filePath) {
#ifdef _WIN32
        handle_ = CreateFileW(utf8ToWide(filePath).c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov = {0};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
            CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; return false;
        }
        owned_ = true; return true;
#else
        fd_ = open(filePath.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) return false;
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) { close(fd_); fd_ = -1; return false; }
        owned_ = true; return true;
#endif
    }

    void unlock() {
        if (!owned_) return;
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov = {0}; UnlockFileEx(handle_, 0, 1, 0, &ov);
            CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); fd_ = -1; }
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
