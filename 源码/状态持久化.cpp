#include "状态持久化.h"
#include "文件锁.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

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

bool StateSerializer::save(const std::string& filePath,
                           const ProcessManager& pm,
                           const MemoryManager& mm,
                           const UserManager& um,
                           const Scheduler& sched) {
    auto out = openOfstream(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    // === 文件头 ===
    writeVal(out, MAGIC);
    writeVal(out, VERSION);
    int64_t timestamp = 0;  // 时间戳(简化处理, 固定为0)
    writeVal(out, timestamp);

    // === 用户数据 ===
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

        // 当前登录用户
        std::string current = um.currentUser();
        writeStr(out, current);
    }

    // === 进程数据 ===
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
            writeVal(out, p.cpuTime);
            writeVal(out, p.burstTime);
            writeVal(out, p.totalMemory);
            writeStr(out, p.owner);

            // 子进程列表
            int32_t childCount = static_cast<int32_t>(p.children.size());
            writeVal(out, childCount);
            for (int32_t c : p.children) {
                writeVal(out, c);
            }

            // 内存块列表
            int32_t blockCount = static_cast<int32_t>(p.memoryBlocks.size());
            writeVal(out, blockCount);
            for (const auto& blk : p.memoryBlocks) {
                writeVal(out, blk.first);
                writeVal(out, blk.second);
            }
        }

        // nextPid
        writeVal(out, pm.nextPid());
    }

    // === 内存数据 ===
    {
        std::lock_guard<std::recursive_mutex> lock(mm.mutex());
        // 空闲块
        const auto& freeBlocks = mm.getFreeBlocks();
        int32_t freeCount = static_cast<int32_t>(freeBlocks.size());
        writeVal(out, freeCount);
        for (const auto& blk : freeBlocks) {
            writeVal(out, blk.startAddr);
            writeVal(out, blk.size);
            writeVal(out, blk.pid);
            writeVal(out, blk.free);
        }

        // 已分配块
        const auto& allocBlocks = mm.getAllocatedBlocks();
        int32_t allocCount = static_cast<int32_t>(allocBlocks.size());
        writeVal(out, allocCount);
        for (const auto& blk : allocBlocks) {
            writeVal(out, blk.startAddr);
            writeVal(out, blk.size);
            writeVal(out, blk.pid);
            writeVal(out, blk.free);
        }

        // 分配算法
        int32_t algo = static_cast<int32_t>(mm.getAllocAlgo());
        writeVal(out, algo);
    }

    // === 调度器数据 ===
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

bool StateSerializer::load(const std::string& filePath,
                           ProcessManager& pm,
                           MemoryManager& mm,
                           UserManager& um,
                           Scheduler& sched) {
    auto in = openIfstream(filePath, std::ios::binary);
    if (!in.is_open()) return false;

    // === 文件头 ===
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

    // 先读到临时变量, 校验通过再替换, 防止读到一半出错搞坏现有状态

    // 用户数据
    std::unordered_map<std::string, UserManager::UserInfo> users;
    std::string currentUser;
    {
        int32_t userCount = 0;
        readVal(in, userCount);
        if (in.fail() || userCount < 0 || userCount > 10000) {
            std::cerr << "[持久化] 用户数量异常: " << userCount << "\n";
            return false;
        }
        for (int32_t i = 0; i < userCount; i++) {
            UserManager::UserInfo info;
            info.username = readStr(in);
            info.passwordHash = readStr(in);
            readVal(in, info.failedAttempts);
            readVal(in, info.locked);
            if (in.fail()) { std::cerr << "[持久化] 读取用户数据失败\n"; return false; }
            users[info.username] = info;
        }
        currentUser = readStr(in);
    }

    // 进程数据
    std::unordered_map<int32_t, PCB> loadedPcbs;
    int32_t loadedNextPid;
    {
        int32_t pcbCount = 0;
        readVal(in, pcbCount);
        if (in.fail() || pcbCount < 0 || pcbCount > 50000) {
            std::cerr << "[持久化] 进程数量异常: " << pcbCount << "\n";
            return false;
        }
        for (int32_t i = 0; i < pcbCount; i++) {
            PCB p;
            readVal(in, p.pid);
            readVal(in, p.ppid);
            p.name = readStr(in);
            int32_t stateVal;
            readVal(in, stateVal);
            p.state = static_cast<PCB::State>(stateVal);
            readVal(in, p.priority);
            readVal(in, p.cpuTime);
            readVal(in, p.burstTime);
            readVal(in, p.totalMemory);
            p.owner = readStr(in);
            if (in.fail()) { std::cerr << "[持久化] 读取PCB数据失败\n"; return false; }

            int32_t childCount = 0;
            readVal(in, childCount);
            if (childCount < 0 || childCount > 10000) return false;
            p.children.resize(childCount);
            for (int32_t j = 0; j < childCount; j++) {
                readVal(in, p.children[j]);
            }

            int32_t blockCount = 0;
            readVal(in, blockCount);
            if (blockCount < 0 || blockCount > 10000) return false;
            p.memoryBlocks.resize(blockCount);
            for (int32_t j = 0; j < blockCount; j++) {
                readVal(in, p.memoryBlocks[j].first);
                readVal(in, p.memoryBlocks[j].second);
            }
            if (in.fail()) { std::cerr << "[持久化] 读取PCB子数据失败\n"; return false; }

            loadedPcbs[p.pid] = p;
        }
        readVal(in, loadedNextPid);
    }

    // 内存数据
    std::list<MemoryManager::MemBlock> loadedFreeBlocks, loadedAllocBlocks;
    MemoryManager::AllocAlgo loadedAlgo;
    {
        int32_t freeCount = 0;
        readVal(in, freeCount);
        if (in.fail() || freeCount < 0 || freeCount > 50000) {
            std::cerr << "[持久化] 空闲块数量异常\n"; return false;
        }
        for (int32_t i = 0; i < freeCount; i++) {
            int32_t startAddr = 0, size = 0, pid = -1;
            bool fr = true;
            readVal(in, startAddr);
            readVal(in, size);
            readVal(in, pid);
            readVal(in, fr);
            if (in.fail()) { std::cerr << "[持久化] 读取空闲块失败\n"; return false; }
            loadedFreeBlocks.emplace_back(startAddr, size, pid, fr);
        }

        int32_t allocCount = 0;
        readVal(in, allocCount);
        if (in.fail() || allocCount < 0 || allocCount > 50000) {
            std::cerr << "[持久化] 已分配块数量异常\n"; return false;
        }
        for (int32_t i = 0; i < allocCount; i++) {
            int32_t startAddr = 0, size = 0, pid = -1;
            bool fr = false;
            readVal(in, startAddr);
            readVal(in, size);
            readVal(in, pid);
            readVal(in, fr);
            if (in.fail()) { std::cerr << "[持久化] 读取已分配块失败\n"; return false; }
            loadedAllocBlocks.emplace_back(startAddr, size, pid, fr);
        }

        int32_t algo = 0;
        readVal(in, algo);
        loadedAlgo = static_cast<MemoryManager::AllocAlgo>(algo);
    }

    // 调度器数据
    std::deque<int32_t> loadedQs[3];
    bool savedSchedRunning = false;
    {
        for (int i = 0; i < 3; i++) {
            int32_t count = 0;
            readVal(in, count);
            if (in.fail() || count < 0 || count > 50000) {
                std::cerr << "[持久化] 队列数量异常\n"; return false;
            }
            for (int32_t j = 0; j < count; j++) {
                int32_t pid;
                readVal(in, pid);
                if (in.fail()) return false;
                loadedQs[i].push_back(pid);
            }
        }
        readVal(in, savedSchedRunning);
    }

    // 清理调度队列中的死PID(已被删除的进程)
    for (int i = 0; i < 3; i++) {
        loadedQs[i].erase(
            std::remove_if(loadedQs[i].begin(), loadedQs[i].end(),
                [&loadedPcbs](int32_t pid) { return loadedPcbs.find(pid) == loadedPcbs.end(); }),
            loadedQs[i].end());
    }

    // 清空状态前先暂停调度器, 防止加载中途调度器看到不完整数据
    bool schedWasRunning = sched.isRunning();
    if (schedWasRunning) {
        sched.stop();
    }

    // 所有数据校验通过, 替换现有状态
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

    // 恢复调度器(如果保存时调度器正在运行)
    if (savedSchedRunning) {
        sched.start();
    }

    std::cout << "[持久化] 状态已从文件恢复\n";
    return true;
}

bool StateSerializer::fileExists(const std::string& filePath) {
    auto f = openIfstream(filePath);
    return f.good();
}
