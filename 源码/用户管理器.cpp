// ============================================================
// 用户管理器 — 注册/登录/登出
//
// 密码用 FNV-1a 哈希存储（不是明文），
// 连续输错 3 次锁定账户。
//
// 这是课设中"账户管理"模块（8分）的实现。
// ============================================================

#include "用户管理器.h"
#include <algorithm>

/**
 * hashPassword — FNV-1a 非加密哈希
 *
 * 为什么用哈希？
 *   即使状态文件被人看到，也拿不到原始密码。
 *
 * FNV-1a 算法：
 *   初始值 0x811C9DC5，每字节异或后乘以 0x01000193，
 *   最后转 8 位十六进制字符串。
 *
 * 注意：这是课程设计用的简单哈希，不是加密，
 * 实际系统应该用 bcrypt 等专用密码哈希函数。
 */
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

/**
 * registerUser — 注册新用户
 *
 * 流程：
 *   1. 检查用户名是否已存在
 *   2. 密码 FNV-1a 哈希后存储
 *   3. failedAttempts=0, locked=false
 */
bool UserManager::registerUser(const std::string& username, const std::string& password) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (users_.find(username) != users_.end()) {
        return false;  // 用户名已存在
    }

    users_.emplace(username, UserInfo(username, password));
    return true;
}

/**
 * login — 用户登录验证
 *
 * 流程：
 *   1. 检查用户是否存在
 *   2. 检查是否被锁定
 *   3. 哈希比对密码
 *   4. 失败 → failedAttempts++，≥3 次锁定
 *   5. 成功 → 重置失败计数，记录 currentUser
 */
bool UserManager::login(const std::string& username, const std::string& password,
                         std::string& errorMsg) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        errorMsg = "用户不存在";
        return false;
    }

    UserInfo& info = it->second;

    if (info.locked) {
        errorMsg = "账户已被锁定，请联系管理员";
        return false;
    }

    std::string inputHash = hashPassword(password);
    if (inputHash != info.passwordHash) {
        info.failedAttempts++;
        if (info.failedAttempts >= MAX_FAILED_ATTEMPTS) {
            info.locked = true;
            errorMsg = "密码错误！已连续失败" +
                       std::to_string(MAX_FAILED_ATTEMPTS) + "次，账户已锁定";
        } else {
            errorMsg = "密码错误！剩余尝试次数: " +
                       std::to_string(MAX_FAILED_ATTEMPTS - info.failedAttempts);
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
