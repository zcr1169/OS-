// ============================================================
// 内存管理器 — 头文件声明
// ============================================================
#pragma once
#include <vector>
#include <list>
#include <string>
#include <mutex>
#include <cstdint>

/**
 * MemoryManager — 动态分区内存分配
 *
 * 数据结构：
 *   freeBlocks_  — 空闲块链表（list<MemBlock>）
 *   allocBlocks_ — 已分配块链表（list<MemBlock>）
 *
 * 为什么用 list 而不是 vector？
 *   分配和释放需要频繁插入/删除，list 是 O(1)，
 *   vector 插入是 O(n)。
 *
 * 特殊 PID 常量：
 *   PID_DATA = -2   → 数据区（alloc data）
 *   PID_IO   = -3   → IO 区（alloc io）
 *   PID_KERNEL = -4 → 内核区（alloc kernel）
 */
class MemoryManager {
public:
    struct MemBlock {
        int32_t startAddr;  // 起始地址(KB)
        int32_t size;       // 大小(KB)
        int32_t pid;        // 所属进程PID, -1空闲, -2数据, -3IO, -4内核
        bool free;          // 是否空闲

        MemBlock(int32_t s, int32_t sz, int32_t p, bool f)
            : startAddr(s), size(sz), pid(p), free(f) {}
    };

    enum AllocAlgo {
        FIRST_FIT = 0,   // 首次适应
        BEST_FIT = 1,    // 最佳适应
        WORST_FIT = 2    // 最坏适应
    };

    static const int32_t PID_DATA = -2;    // 数据
    static const int32_t PID_IO = -3;      // IO
    static const int32_t PID_KERNEL = -4;  // 内核

    MemoryManager();
    explicit MemoryManager(int32_t totalSize);

    int32_t alloc(int32_t size, int32_t pid);
    void freeByPid(int32_t pid);
    bool freeByAddr(int32_t addr);
    int32_t getPidByAddr(int32_t addr) const;

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
