// 状态持久化 — 二进制 save/load，全量序列化系统状态
#include "状态持久化.h"
#include "文件锁.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>

static std::ofstream openOfstream(const std::string& utf8Path, std::ios_base::openmode mode = std::ios::binary) {
    return std::ofstream(utf8ToWide(utf8Path), mode);
}
static std::ifstream openIfstream(const std::string& utf8Path, std::ios_base::openmode mode = std::ios::binary) {
    return std::ifstream(utf8ToWide(utf8Path), mode);
}
#else
static std::ofstream openOfstream(const std::string& path, std::ios_base::openmode mode = std::ios::binary) {
    return std::ofstream(path, mode);
}
static std::ifstream openIfstream(const std::string& path, std::ios_base::openmode mode = std::ios::binary) {
    return std::ifstream(path, mode);
}
#endif

// save — 保存：文件头→用户→进程(含children+memoryBlocks)→内存→调度队列
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

    {   // 用户
        std::lock_guard<std::recursive_mutex> lock(um.mutex());
        const auto& users = um.getUsers();
        writeVal(out, (int32_t)users.size());
        for (const auto& pair : users) {
            const auto& info = pair.second;
            writeStr(out, info.username);
            writeStr(out, info.passwordHash);
            writeVal(out, info.failedAttempts);
            writeVal(out, info.locked);
        }
        writeStr(out, um.currentUser());
    }

    {   // 进程
        std::lock_guard<std::recursive_mutex> lock(pm.mutex());
        const auto& pcbs = pm.getAllPCBs();
        writeVal(out, (int32_t)pcbs.size());
        for (const auto& pair : pcbs) {
            const PCB& p = pair.second;
            writeVal(out, p.pid); writeVal(out, p.ppid); writeStr(out, p.name);
            writeVal(out, (int32_t)p.state); writeVal(out, p.priority);
            writeVal(out, p.cpuTime); writeVal(out, p.burstTime);
            writeVal(out, p.totalMemory); writeStr(out, p.owner);
            writeVal(out, (int32_t)p.children.size());
            for (int32_t c : p.children) writeVal(out, c);
            writeVal(out, (int32_t)p.memoryBlocks.size());
            for (const auto& blk : p.memoryBlocks) { writeVal(out, blk.first); writeVal(out, blk.second); }
        }
        writeVal(out, pm.nextPid());
    }

    {   // 内存
        std::lock_guard<std::recursive_mutex> lock(mm.mutex());
        const auto& freeBlocks = mm.getFreeBlocks();
        writeVal(out, (int32_t)freeBlocks.size());
        for (const auto& blk : freeBlocks) { writeVal(out, blk.startAddr); writeVal(out, blk.size); writeVal(out, blk.pid); writeVal(out, blk.free); }
        const auto& allocBlocks = mm.getAllocatedBlocks();
        writeVal(out, (int32_t)allocBlocks.size());
        for (const auto& blk : allocBlocks) { writeVal(out, blk.startAddr); writeVal(out, blk.size); writeVal(out, blk.pid); writeVal(out, blk.free); }
        writeVal(out, (int32_t)mm.getAllocAlgo());
    }

    {   // 调度队列
        std::lock_guard<std::mutex> lock(sched.mutex());
        for (int i = 0; i < 3; i++) {
            const auto& q = sched.getQueue(i);
            writeVal(out, (int32_t)q.size());
            for (int32_t pid : q) writeVal(out, pid);
        }
        writeVal(out, sched.isRunning());
    }

    out.close();
    return true;
}

// load — 恢复：先读到临时变量，校验通过再替换现有状态
bool StateSerializer::load(const std::string& filePath,
                           ProcessManager& pm,
                           MemoryManager& mm,
                           UserManager& um,
                           Scheduler& sched) {
    auto in = openIfstream(filePath, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0, version = 0;
    readVal(in, magic); readVal(in, version);
    if (magic != MAGIC) { std::cerr << "[持久化] 文件格式错误: 魔数不匹配\n"; return false; }
    if (version != VERSION) { std::cerr << "[持久化] 文件版本不兼容: " << version << "\n"; return false; }

    int64_t timestamp; readVal(in, timestamp);

    std::unordered_map<std::string, UserManager::UserInfo> users;
    std::string currentUser;
    std::unordered_map<int32_t, PCB> loadedPcbs;
    int32_t loadedNextPid;
    std::list<MemoryManager::MemBlock> loadedFreeBlocks, loadedAllocBlocks;
    MemoryManager::AllocAlgo loadedAlgo;
    std::deque<int32_t> loadedQs[3];
    bool savedSchedRunning = false;

    {   int32_t userCount; readVal(in, userCount);
        for (int32_t i = 0; i < userCount; i++) {
            UserManager::UserInfo info;
            info.username = readStr(in); info.passwordHash = readStr(in);
            readVal(in, info.failedAttempts); readVal(in, info.locked);
            users[info.username] = info;
        }
        currentUser = readStr(in);
    }

    {   int32_t pcbCount; readVal(in, pcbCount);
        for (int32_t i = 0; i < pcbCount; i++) {
            PCB p; readVal(in, p.pid); readVal(in, p.ppid); p.name = readStr(in);
            int32_t sv; readVal(in, sv); p.state = (PCB::State)sv;
            readVal(in, p.priority); readVal(in, p.cpuTime); readVal(in, p.burstTime);
            readVal(in, p.totalMemory); p.owner = readStr(in);
            int32_t cc; readVal(in, cc); p.children.resize(cc);
            for (int32_t j = 0; j < cc; j++) readVal(in, p.children[j]);
            int32_t bc; readVal(in, bc); p.memoryBlocks.resize(bc);
            for (int32_t j = 0; j < bc; j++) { readVal(in, p.memoryBlocks[j].first); readVal(in, p.memoryBlocks[j].second); }
            loadedPcbs[p.pid] = p;
        }
        readVal(in, loadedNextPid);
    }

    {   int32_t fc; readVal(in, fc);
        for (int32_t i = 0; i < fc; i++) {
            int32_t a, s, p; bool fr; readVal(in, a); readVal(in, s); readVal(in, p); readVal(in, fr);
            loadedFreeBlocks.emplace_back(a, s, p, fr);
        }
        int32_t ac; readVal(in, ac);
        for (int32_t i = 0; i < ac; i++) {
            int32_t a, s, p; bool fr; readVal(in, a); readVal(in, s); readVal(in, p); readVal(in, fr);
            loadedAllocBlocks.emplace_back(a, s, p, fr);
        }
        int32_t algo; readVal(in, algo); loadedAlgo = (MemoryManager::AllocAlgo)algo;
    }

    {   for (int i = 0; i < 3; i++) {
            int32_t cnt; readVal(in, cnt);
            for (int32_t j = 0; j < cnt; j++) { int32_t pid; readVal(in, pid); loadedQs[i].push_back(pid); }
        }
        readVal(in, savedSchedRunning);
    }

    for (int i = 0; i < 3; i++) {
        loadedQs[i].erase(std::remove_if(loadedQs[i].begin(), loadedQs[i].end(),
            [&loadedPcbs](int32_t pid) { return loadedPcbs.find(pid) == loadedPcbs.end(); }), loadedQs[i].end());
    }

    bool schedWasRunning = sched.isRunning();
    if (schedWasRunning) sched.stop();

    pm.clear(); mm.clear(); um.clear();
    { std::lock_guard<std::recursive_mutex> lock(um.mutex()); um.setUsers(users); um.setCurrentUser(currentUser); }
    { std::lock_guard<std::recursive_mutex> lock(pm.mutex()); pm.getAllPCBsUnsafe() = loadedPcbs; pm.setNextPid(loadedNextPid); }
    { std::lock_guard<std::recursive_mutex> lock(mm.mutex()); mm.setFreeBlocks(loadedFreeBlocks); mm.setAllocatedBlocks(loadedAllocBlocks); mm.setAllocAlgo(loadedAlgo); }
    { std::lock_guard<std::mutex> lock(sched.mutex()); for (int i = 0; i < 3; i++) sched.setQueue(i, loadedQs[i]); }

    if (savedSchedRunning) sched.start();
    std::cout << "[持久化] 状态已从文件恢复\n";
    return true;
}

bool StateSerializer::fileExists(const std::string& filePath) {
    auto f = openIfstream(filePath);
    return f.good();
}
