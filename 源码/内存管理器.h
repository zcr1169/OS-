#pragma once
#include <vector>
#include <list>
#include <string>
#include <mutex>
#include <cstdint>

// 内存管理器 - 动态分区分配, 支持FF/BF/WF三种算法, 总内存1024KB
class MemoryManager {
public:
    // 内存块
    struct MemBlock {
        int32_t startAddr;  // 起始地址(KB)
        int32_t size;       // 大小(KB)
        int32_t pid;        // 所属进程PID, -1表示空闲
        bool free;          // 是否空闲

        MemBlock(int32_t s, int32_t sz, int32_t p, bool f)
            : startAddr(s), size(sz), pid(p), free(f) {}
    };

    // 分配算法
    enum AllocAlgo {
        FIRST_FIT = 0,   // 首次适应
        BEST_FIT = 1,    // 最佳适应
        WORST_FIT = 2    // 最坏适应
    };

    MemoryManager();
    explicit MemoryManager(int32_t totalSize);

    // 分配内存, 返回起始地址, 失败返回-1
    int32_t alloc(int32_t size, int32_t pid);

    // 释放指定进程的所有内存
    void freeByPid(int32_t pid);

    // 释放指定地址的内存块
    bool freeByAddr(int32_t addr);

    // 查询地址所属PID, -1表示未分配
    int32_t getPidByAddr(int32_t addr) const;

    // 显示内存使用情况
    std::string showMem() const;

    // 内存紧缩(合并相邻空闲块)
    void compact();

    // 内存碎片统计
    std::string memStat() const;

    // 特殊PID常量(非进程目标)
    static const int32_t PID_DATA = -2;    // 数据
    static const int32_t PID_IO = -3;      // IO
    static const int32_t PID_KERNEL = -4;  // 内核

    // 获取内存块所属者的显示标签
    static std::string getOwnerLabel(int32_t pid);

    // 切换分配算法
    void setAllocAlgo(AllocAlgo algo);
    AllocAlgo getAllocAlgo() const { return algo_; }
    std::string getAllocAlgoName() const;

    // 获取空闲/已分配块列表
    const std::list<MemBlock>& getFreeBlocks() const { return freeBlocks_; }
    const std::list<MemBlock>& getAllocatedBlocks() const { return allocBlocks_; }

    // 获取所有内存块(用于快照)
    std::list<MemBlock> getAllBlocks() const;

    // 总内存大小
    int32_t totalSize() const { return totalSize_; }

    // 设置空闲/已分配块(load恢复用)
    void setFreeBlocks(const std::list<MemBlock>& blocks);
    void setAllocatedBlocks(const std::list<MemBlock>& blocks);

    // 清空
    void clear();

    // 锁
    std::recursive_mutex& mutex() const { return mtx_; }

private:
    // 内部紧缩(调用者需已持有锁)
    void compactInternal();

    int32_t totalSize_;             // 总内存大小(KB)
    std::list<MemBlock> freeBlocks_;   // 空闲块链表
    std::list<MemBlock> allocBlocks_;  // 已分配块链表
    AllocAlgo algo_;                   // 当前分配算法
    mutable std::recursive_mutex mtx_;
};
