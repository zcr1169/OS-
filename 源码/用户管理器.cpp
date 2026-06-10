// 用户管理器 — 注册/登录/登出，密码FNV-1a哈希，3次错误锁定
#include "用户管理器.h"
#include <algorithm>

std::string UserManager::hashPassword(const std::string& password) {
    uint32_t hash = 0x811C9DC5;
    for (char c : password) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x01000193;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", hash);
    return std::string(buf);
}

UserManager::UserManager() {}

bool UserManager::registerUser(const std::string& username, const std::string& password) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (users_.find(username) != users_.end()) return false;
    users_.emplace(username, UserInfo(username, password));
    return true;
}

bool UserManager::login(const std::string& username, const std::string& password,
                         std::string& errorMsg) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = users_.find(username);
    if (it == users_.end()) { errorMsg = "用户不存在"; return false; }

    UserInfo& info = it->second;
    if (info.locked) { errorMsg = "账户已被锁定，请联系管理员"; return false; }

    std::string inputHash = hashPassword(password);
    if (inputHash != info.passwordHash) {
        info.failedAttempts++;
        if (info.failedAttempts >= MAX_FAILED_ATTEMPTS) {
            info.locked = true;
            errorMsg = "密码错误！已连续失败" + std::to_string(MAX_FAILED_ATTEMPTS) + "次，账户已锁定";
        } else {
            errorMsg = "密码错误！剩余尝试次数: " + std::to_string(MAX_FAILED_ATTEMPTS - info.failedAttempts);
        }
        return false;
    }
    info.failedAttempts = 0;
    currentUser_ = username;
    return true;
}

void UserManager::logout() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    currentUser_.clear();
}

void UserManager::setUsers(const std::unordered_map<std::string, UserInfo>& users) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    users_ = users;
}

void UserManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    users_.clear();
    currentUser_.clear();
}
