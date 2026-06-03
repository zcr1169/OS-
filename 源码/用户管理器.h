#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// 用户管理器 - 注册/登录/登出, 含密码哈希和登录锁定
class UserManager {
public:
    struct UserInfo {
        std::string username;
        std::string passwordHash;  // 简单哈希(课程设计用的, 别当真)
        int32_t failedAttempts;    // 连续输错次数
        bool locked;               // 是否锁定

        UserInfo() : failedAttempts(0), locked(false) {}
        UserInfo(const std::string& u, const std::string& p)
            : username(u), passwordHash(hashPassword(p)), failedAttempts(0), locked(false) {}
    };

    UserManager();

    // 注册, 用户名已存在返回false
    bool registerUser(const std::string& username, const std::string& password);

    // 登录验证, 输错3次锁定
    bool login(const std::string& username, const std::string& password,
               std::string& errorMsg);

    // 登出
    void logout();

    // 当前登录用户
    std::string currentUser() const {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        return currentUser_;
    }

    // 是否已登录
    bool isLoggedIn() const {
        return !currentUser().empty();
    }

    // 获取所有用户(只读)
    const std::unordered_map<std::string, UserInfo>& getUsers() const { return users_; }

    // 设置用户数据(load恢复用)
    void setUsers(const std::unordered_map<std::string, UserInfo>& users);

    // 设置当前用户(load恢复用)
    void setCurrentUser(const std::string& user) { currentUser_ = user; }

    // 清空
    void clear();

    // 密码哈希
    static std::string hashPassword(const std::string& password);

    // 锁
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    std::unordered_map<std::string, UserInfo> users_;
    std::string currentUser_;  // 当前登录用户(空表示未登录)
    mutable std::recursive_mutex mtx_;

    static const int32_t MAX_FAILED_ATTEMPTS = 3;
};
