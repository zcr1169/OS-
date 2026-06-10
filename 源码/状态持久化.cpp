// ============================================================
// 状态持久化 — save 和 load 的二进制序列化
//
// 这是课设中"持久化存储"模块（20分）的实现。
//
// PPT 扣分点提醒：
//   ⚠️ 必须使用二进制文件！不能用文本文件！
//   ⚠️ 必须保存动态数据（CPU时间、队列顺序），
//      不能只存 PCB 静态信息！
//   ⚠️ 重启后必须能精准恢复现场！
//
// 文件格式（二进制）：
//   [魔数 0x4F535353][版本号][时间戳]
//   [用户数据] [进程数据] [内存数据] [调度数据]
//
// 保存全部状态：用户、进程（含子进程列表和内存块）、
// 空闲块/已分配块、调度队列顺序、调度器运行状态
// ============================================================

#include "状态持久化.h"
#include "文件锁.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

/** 用 UTF-16 路径打开文件（支持中文路径） */
static std::ofstream openOfstream(const std::string& utf8Path,
                                   std::ios_base::openmode mode = std::ios::binary) {
    return std::ofstream(utf8ToWide(utf8Path), mode);
}

static std::ifstream openIfstream(const std::string& utf8Path,
                                   std::ios_base::openmode mode = std::ios::binary) {
    return std::ifstream(utf8ToWide(utf8Path), mode);
}
#else
static std::ofstream openOfstream(const std::string& path,
                                   std::ios_base::openmode mode = std::ios::binary) {
    return std::ofstream(path, mode);
}
static std::ifstream openIfstream(const std::string& path,
                                   std::ios_base::openmode mode = std::ios::binary) {
    return std::ifstream(path, mode);
}
#endif

/**
 * save — 保存全量状态到二进制文件
 *
 * 逐模块序列化：
 *   1. 文件头：魔数 + 版本 + 时间戳
 *   2. 用户数据：数量、用户名、哈希、失败次数、锁定状态、当前登录用户
 *   3. 进程数据：数量、每个 PCB 的全部字段（含 children 和 memoryBlocks）
 *   4. 内存数据：空闲块、已分配块、分配算法
 *   5. 调度器数据：三个队列的 PID 顺序、运行标志
 *
 * 每个模块先加锁，再序列化，确保数据一致性。
 */
bool StateSerializer::save(const std::string& filePath,
                           const ProcessManager& pm,
                           const MemoryManager& mm,
                           const UserManager& um,
                           const Scheduler& sched) {
    auto out = openOfstream(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    writeVal(out, MAGIC);
    writeVal(out, VERSION);
    int64_t timestamp = 0;
    writeVal(out, timestamp);

    // 用户数据
    {
        std::lock_guard<std::recursive_mutex> lock(um.mutex());
        const auto& users = um.getUsers();
        int32_t userCount = static_cast<int32_t>(users.size());
        writeVal(out, userCount);

        for (const auto& pair : users) {
            const auto& info = pair.second;
            writeStr(out, info.username);
            writeStr(out, info.passwordHash);
            writeVal(out, info.failedAttempts);
            writeVal(out, info.locked);
        }
        writeStr(out, um.currentUser());  // 保存当前登录用户
    }

    // 进程数据
    {
        std::lock_guard<std::recursive_mutex> lock(pm.mutex());
        const auto& pcbs = pm.getAllPCBs();
        int32_t pcbCount = static_cast<int32_t>(pcbs.size());
        writeVal(out, pcbCount);

        for (const auto& pair : pcbs) {
            const PCB& p = pair.second;
            writeVal(out, p.pid);
            writeVal(out, p.ppid);
            writeStr(out, p.name);
            writeVal(out, static_cast<int32_t>(p.state));
            writeVal(out, p.priority);
            writeVal(out, p.cpuTime);     // ⚠️ 保存动态数据！
            writeVal(out, p.burstTime);
            writeVal(out, p.totalMemory);
            writeStr(out, p.owner);

            // 子进程列表（恢复 ptree 所需）
            int32_t childCount = static_cast<int32_t>(p.children.size());
            writeVal(out, childCount);
            for (int32_t c : p.children) {
                writeVal(out, c);
            }

            // 内存块列表（恢复进程内存信息）
            int32_t blockCount = static_cast<int32_t>(p.memoryBlocks.size());
            writeVal(out, blockCount);
            for (const auto& blk : p.memoryBlocks) {
                writeVal(out, blk.first);
                writeVal(out, blk.second);
            }
        }
        writeVal(out, pm.nextPid());
    }

    // 内存数据
    {
        std::lock_guard<std::recursive_mutex> lock(mm.mutex());
        const auto& freeBlocks = mm.getFreeBlocks();
        int32_t freeCount = static_cast<int32_t>(freeBlocks.size());
        writeVal(out, freeCount);
        for (const auto& blk : freeBlocks) {
            writeVal(out, blk.startAddr);
            writeVal(out, blk.size);
            writeVal(out, blk.pid);
            writeVal(out, blk.free);
        }

        const auto& allocBlocks = mm.getAllocatedBlocks();
        int32_t allocCount = static_cast<int32_t>(allocBlocks.size());
        writeVal(out, allocCount);
        for (const auto& blk : allocBlocks) {
            writeVal(out, blk.startAddr);
            writeVal(out, blk.size);
            writeVal(out, blk.pid);
            writeVal(out, blk.free);
        }

        int32_t algo = static_cast<int32_t>(mm.getAllocAlgo());
        writeVal(out, algo);
    }

    // 调度器数据
    {
        std::lock_guard<std::mutex> lock(sched.mutex());
        for (int i = 0; i < 3; i++) {
            const auto& q = sched.getQueue(i);
            int32_t count = static_cast<int32_t>(q.size());
            writeVal(out, count);
            for (int32_t pid : q) {
                writeVal(out, pid);
            }
        }
        writeVal(out, sched.isRunning());
    }

    out.close();
    return true;
}

/**
 * load — 从二进制文件恢复全部状态
 *
 * 安全设计：
 *   1. 先读取到临时变量，校验通过后再替换现有状态
 *   2. 如果文件损坏（魔数不对/版本不兼容），拒绝加载
 *   3. 加载前暂停调度器，防止看到不完整数据
 *   4. 清理调度队列中的死 PID（已被删除但队列残留的）
 */
bool StateSerializer::load(const std::string& filePath,
                           ProcessManager& pm,
                           MemoryManager& mm,
                           UserManager& um,
                           Scheduler& sched) {
    auto in = openIfstream(filePath, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0, version = 0;
    readVal(in, magic);
    readVal(in, version);

    if (magic != MAGIC) {
        std::cerr << "[持久化] 文件格式错误: 魔数不匹配\n";
        return false;
    }
    if (version != VERSION) {
        std::cerr << "[持久化] 文件版本不兼容: " << version << "\n";
        return false;
    }

    int64_t timestamp;
    readVal(in, timestamp);

    // ---- 临时变量，校验通过才替换 ----
    std::unordered_map<std::string, UserManager::UserInfo> users;
    std::string currentUser;
    std::unordered_map<int32_t, PCB> loadedPcbs;
    int32_t loadedNextPid;
    std::list<MemoryManager::MemBlock> loadedFreeBlocks, loadedAllocBlocks;
    MemoryManager::AllocAlgo loadedAlgo;
    std::deque<int32_t> loadedQs[3];
    bool savedSchedRunning = false;

    // 读取用户数据
    {
        int32_t userCount = 0;
        readVal(in, userCount);
        if (in.fail() || userCount < 0 || userCount > 10000) return false;
        for (int32_t i = 0; i < userCount; i++) {
            UserManager::UserInfo info;
            info.username = readStr(in);
            info.passwordHash = readStr(in);
            readVal(in, info.failedAttempts);
            readVal(in, info.locked);
            if (in.fail()) return false;
            users[info.username] = info;
        }
        currentUser = readStr(in);
    }

    // 读取进程数据
    {
        int32_t pcbCount = 0;
        readVal(in, pcbCount);
        if (in.fail() || pcbCount < 0 || pcbCount > 50000) return false;
        for (int32_t i = 0; i < pcbCount; i++) {
            PCB p;
            readVal(in, p.pid);
            readVal(in, p.ppid);
            p.name = readStr(in);
            int32_t stateVal; readVal(in, stateVal);
            p.state = static_cast<PCB::State>(stateVal);
            readVal(in, p.priority);
            readVal(in, p.cpuTime);
            readVal(in, p.burstTime);
            readVal(in, p.totalMemory);
            p.owner = readStr(in);
            if (in.fail()) return false;

            int32_t childCount; readVal(in, childCount);
            if (childCount < 0 || childCount > 10000) return false;
            p.children.resize(childCount);
            for (int32_t j = 0; j < childCount; j++) readVal(in, p.children[j]);

            int32_t blockCount; readVal(in, blockCount);
            if (blockCount < 0 || blockCount > 10000) return false;
            p.memoryBlocks.resize(blockCount);
            for (int32_t j = 0; j < blockCount; j++) {
                readVal(in, p.memoryBlocks[j].first);
                readVal(in, p.memoryBlocks[j].second);
            }
            if (in.fail()) return false;
            loadedPcbs[p.pid] = p;
        }
        readVal(in, loadedNextPid);
    }

    // 读取内存数据
    {
        int32_t freeCount; readVal(in, freeCount);
        if (in.fail() || freeCount < 0 || freeCount > 50000) return false;
        for (int32_t i = 0; i < freeCount; i++) {
            int32_t startAddr, size, pid; bool fr;
            readVal(in, startAddr); readVal(in, size);
            readVal(in, pid); readVal(in, fr);
            if (in.fail()) return false;
            loadedFreeBlocks.emplace_back(startAddr, size, pid, fr);
        }

        int32_t allocCount; readVal(in, allocCount);
        if (in.fail() || allocCount < 0 || allocCount > 50000) return false;
        for (int32_t i = 0; i < allocCount; i++) {
            int32_t startAddr, size, pid; bool fr;
            readVal(in, startAddr); readVal(in, size);
            readVal(in, pid); readVal(in, fr);
            if (in.fail()) return false;
            loadedAllocBlocks.emplace_back(startAddr, size, pid, fr);
        }

        int32_t algo; readVal(in, algo);
        loadedAlgo = static_cast<MemoryManager::AllocAlgo>(algo);
    }

    // 读取调度器数据
    {
        for (int i = 0; i < 3; i++) {
            int32_t count; readVal(in, count);
            if (in.fail() || count < 0 || count > 50000) return false;
            for (int32_t j = 0; j < count; j++) {
                int32_t pid; readVal(in, pid);
                if (in.fail()) return false;
                loadedQs[i].push_back(pid);
            }
        }
        readVal(in, savedSchedRunning);
    }

    // 清理调度队列中的死 PID
    for (int i = 0; i < 3; i++) {
        loadedQs[i].erase(
            std::remove_if(loadedQs[i].begin(), loadedQs[i].end(),
                [&loadedPcbs](int32_t pid) { return loadedPcbs.find(pid) == loadedPcbs.end(); }),
            loadedQs[i].end());
    }

    // ---- 校验通过，替换现有状态 ----
    bool schedWasRunning = sched.isRunning();
    if (schedWasRunning) sched.stop();

    pm.clear();
    mm.clear();
    um.clear();

    {
        std::lock_guard<std::recursive_mutex> lock(um.mutex());
        um.setUsers(users);
        um.setCurrentUser(currentUser);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(pm.mutex());
        pm.getAllPCBsUnsafe() = loadedPcbs;
        pm.setNextPid(loadedNextPid);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mm.mutex());
        mm.setFreeBlocks(loadedFreeBlocks);
        mm.setAllocatedBlocks(loadedAllocBlocks);
        mm.setAllocAlgo(loadedAlgo);
    }
    {
        std::lock_guard<std::mutex> lock(sched.mutex());
        for (int i = 0; i < 3; i++) {
            sched.setQueue(i, loadedQs[i]);
        }
    }

    if (savedSchedRunning) sched.start();

    std::cout << "[持久化] 状态已从文件恢复\n";
    return true;
}

bool StateSerializer::fileExists(const std::string& filePath) {
    auto f = openIfstream(filePath);
    return f.good();
}
