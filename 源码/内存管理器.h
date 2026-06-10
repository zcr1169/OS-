// 内存管理器 — 头文件
#pragma once
#include <vector>
#include <list>
#include <string>
#include <mutex>
#include <cstdint>

// 动态分区分配，支持FF/BF/WF三种算法
// 可分配给进程(PID>0)、数据(PID=-2)、IO(PID=-3)、内核(PID=-4)
class MemoryManager {
public:
    struct MemBlock {
        int32_t startAddr, size, pid;
        bool free;
        MemBlock(int32_t s, int32_t sz, int32_t p, bool f)
            : startAddr(s), size(sz), pid(p), free(f) {}
    };

    enum AllocAlgo { FIRST_FIT = 0, BEST_FIT = 1, WORST_FIT = 2 };

    static const int32_t PID_DATA = -2;
    static const int32_t PID_IO = -3;
    static const int32_t PID_KERNEL = -4;
    static const int32_t PID_SWAPPED = -5;  // 已换出到交换空间的内存块

    MemoryManager();
    explicit MemoryManager(int32_t totalSize);

    int32_t alloc(int32_t size, int32_t pid);
    void freeByPid(int32_t pid);
    bool freeByAddr(int32_t addr);
    int32_t getPidByAddr(int32_t addr) const;

    // swap 专用：将进程内存块标记为"换出"（留在 allocBlocks 但 pid 变 PID_SWAPPED）
    void swapOutPid(int32_t pid);
    // swap 专用：从 allocBlocks 移除某进程的"换出"标记块
    void removeSwappedByPid(int32_t pid);

    std::string showMem() const;
    void compact();
    std::string memStat() const;

    void setAllocAlgo(AllocAlgo algo);
    AllocAlgo getAllocAlgo() const { return algo_; }
    std::string getAllocAlgoName() const;

    const std::list<MemBlock>& getFreeBlocks() const { return freeBlocks_; }
    const std::list<MemBlock>& getAllocatedBlocks() const { return allocBlocks_; }
    std::list<MemBlock> getAllBlocks() const;

    int32_t totalSize() const { return totalSize_; }
    void setFreeBlocks(const std::list<MemBlock>& blocks);
    void setAllocatedBlocks(const std::list<MemBlock>& blocks);
    void clear();

    static std::string getOwnerLabel(int32_t pid);
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    void compactInternal();
    int32_t totalSize_;
    std::list<MemBlock> freeBlocks_;
    std::list<MemBlock> allocBlocks_;
    AllocAlgo algo_;
    mutable std::recursive_mutex mtx_;
};
