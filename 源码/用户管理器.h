// 用户管理器 — 头文件
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

class UserManager {
public:
    struct UserInfo {
        std::string username;
        std::string passwordHash;
        int32_t failedAttempts = 0;
        bool locked = false;
        UserInfo() = default;
        UserInfo(const std::string& u, const std::string& p)
            : username(u), passwordHash(hashPassword(p)) {}
    };

    UserManager();
    bool registerUser(const std::string& username, const std::string& password);
    bool login(const std::string& username, const std::string& password, std::string& errorMsg);
    void logout();

    std::string currentUser() const { std::lock_guard<std::recursive_mutex> lock(mtx_); return currentUser_; }
    bool isLoggedIn() const { return !currentUser().empty(); }

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
