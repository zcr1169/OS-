// ============================================================
// 用户管理器 — 头文件声明
// ============================================================
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

/**
 * UserManager — 用户管理
 *
 * 数据结构：
 *   users_ — unordered_map<用户名, UserInfo>
 *   currentUser_ — 当前登录的用户名（空=未登录）
 *
 * 密码安全：
 *   不存明文，存 FNV-1a 哈希后的十六进制字符串
 */
class UserManager {
public:
    struct UserInfo {
        std::string username;
        std::string passwordHash;   // FNV-1a 哈希
        int32_t failedAttempts;     // 连续输错次数
        bool locked;                // 是否锁定

        UserInfo() : failedAttempts(0), locked(false) {}
        UserInfo(const std::string& u, const std::string& p)
            : username(u), passwordHash(hashPassword(p)), failedAttempts(0), locked(false) {}
    };

    UserManager();

    bool registerUser(const std::string& username, const std::string& password);
    bool login(const std::string& username, const std::string& password,
               std::string& errorMsg);
    void logout();

    std::string currentUser() const {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        return currentUser_;
    }

    bool isLoggedIn() const {
        return !currentUser().empty();
    }

    const std::unordered_map<std::string, UserInfo>& getUsers() const { return users_; }
    void setUsers(const std::unordered_map<std::string, UserInfo>& users);
    void setCurrentUser(const std::string& user) { currentUser_ = user; }
    void clear();

    static std::string hashPassword(const std::string& password);
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    std::unordered_map<std::string, UserInfo> users_;
    std::string currentUser_;
    mutable std::recursive_mutex mtx_;

    static const int32_t MAX_FAILED_ATTEMPTS = 3;
};
