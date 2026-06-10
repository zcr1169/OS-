// ============================================================
// 内存管理器 — 动态分区分配
//
// 模拟操作系统中的动态分区内存管理，支持三种分配算法：
//   - 首次适应（FF）：找到第一个够大的空闲块
//   - 最佳适应（BF）：找到最接近请求大小的空闲块
//   - 最坏适应（WF）：找到最大的空闲块
//
// 核心数据结构：
//   freeBlocks_   — 空闲块链表（按地址排序）
//   allocBlocks_  — 已分配块链表
//   总内存 1024KB，初始为一个大空闲块
//
// 支持非进程目标：
//   alloc 可以分配给进程（PID>0），也可以分配给
//   数据（PID=-2）、IO（PID=-3）、内核（PID=-4）
// ============================================================

#include "内存管理器.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

static const int32_t DEFAULT_MEM_SIZE = 1024;  // 默认 1MB

MemoryManager::MemoryManager() : totalSize_(DEFAULT_MEM_SIZE), algo_(FIRST_FIT) {
    freeBlocks_.emplace_back(0, totalSize_, -1, true);  // 初始全空闲
}

MemoryManager::MemoryManager(int32_t totalSize) : totalSize_(totalSize), algo_(FIRST_FIT) {
    freeBlocks_.emplace_back(0, totalSize_, -1, true);
}

/**
 * alloc — 分配内存
 *
 * 算法选择：
 *   FIRST_FIT：遍历空闲块，第一个够大的就分配
 *   BEST_FIT：遍历所有，选 size 最接近的
 *   WORST_FIT：遍历所有，选 size 最大的
 *
 * 分配操作：
 *   如果空闲块恰好等于请求大小 → 整个块转为已分配
 *   如果空闲块大于请求大小 → 分裂：前部分分配，后部分继续空闲
 */
int32_t MemoryManager::alloc(int32_t size, int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (size <= 0) return -1;

    auto selected = freeBlocks_.end();
    int32_t bestDiff = INT32_MAX;
    int32_t worstDiff = -1;

    for (auto it = freeBlocks_.begin(); it != freeBlocks_.end(); ++it) {
        if (it->size < size) continue;

        switch (algo_) {
            case FIRST_FIT:
                selected = it;
                goto found;
            case BEST_FIT: {
                int32_t diff = it->size - size;
                if (diff < bestDiff) { bestDiff = diff; selected = it; }
                break;
            }
            case WORST_FIT: {
                int32_t diff = it->size - size;
                if (diff > worstDiff) { worstDiff = diff; selected = it; }
                break;
            }
        }
    }

    if (algo_ != FIRST_FIT && selected == freeBlocks_.end()) return -1;
    if (selected == freeBlocks_.end()) return -1;

found:
    int32_t allocAddr = selected->startAddr;
    if (selected->size == size) {
        allocBlocks_.emplace_back(selected->startAddr, size, pid, false);
        freeBlocks_.erase(selected);
    } else {
        allocBlocks_.emplace_back(selected->startAddr, size, pid, false);
        selected->startAddr += size;
        selected->size -= size;
    }
    return allocAddr;
}

/** freeByPid — 释放指定进程的所有内存块（kill/swap_out/自动终止时调用） */
void MemoryManager::freeByPid(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    std::vector<std::list<MemBlock>::iterator> toFree;
    for (auto it = allocBlocks_.begin(); it != allocBlocks_.end(); ++it) {
        if (it->pid == pid) {
            toFree.push_back(it);
        }
    }

    for (auto it : toFree) {
        int32_t addr = it->startAddr;
        int32_t size = it->size;
        allocBlocks_.erase(it);
        auto fit = freeBlocks_.begin();
        while (fit != freeBlocks_.end() && fit->startAddr < addr) ++fit;
        freeBlocks_.insert(fit, MemBlock(addr, size, -1, true));
    }
    compactInternal();  // 释放后自动合并相邻空闲块
}

/** freeByAddr — 按地址释放指定内存块（free_mem 命令） */
bool MemoryManager::freeByAddr(int32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    for (auto it = allocBlocks_.begin(); it != allocBlocks_.end(); ++it) {
        if (it->startAddr == addr) {
            int32_t size = it->size;
            allocBlocks_.erase(it);
            auto fit = freeBlocks_.begin();
            while (fit != freeBlocks_.end() && fit->startAddr < addr) ++fit;
            freeBlocks_.insert(fit, MemBlock(addr, size, -1, true));
            compactInternal();
            return true;
        }
    }
    return false;
}

/** getPidByAddr — 查询某地址属于哪个进程 */
int32_t MemoryManager::getPidByAddr(int32_t addr) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (const auto& blk : allocBlocks_) {
        if (blk.startAddr == addr) return blk.pid;
    }
    return -1;
}

/**
 * showMem — 显示内存使用情况
 *
 * 输出三个区块：
 *   1. 空闲块列表 — 所有空闲块
 *   2. 已分配块列表 — 所有已分配块（含所属者）
 *   3. 内存分配图 — ASCII 字符画内存条
 *   4. 内存块总览 — 按地址排序统一列出，标注类型
 */
std::string MemoryManager::showMem() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    std::ostringstream oss;
    oss << "===== 内存使用情况 =====\n";
    oss << "总内存: " << totalSize_ << " KB\n";
    oss << "分配算法: " << getAllocAlgoName() << "\n\n";

    if (freeBlocks_.empty() && allocBlocks_.empty()) {
        oss << "内存为空\n";
        return oss.str();
    }

    // 空闲块列表
    oss << "【空闲块列表】\n";
    oss << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)\n";
    oss << std::string(24, '-') << "\n";
    int32_t totalFree = 0;
    for (const auto& blk : freeBlocks_) {
        oss << std::setw(8) << blk.startAddr << "KB"
            << std::setw(12) << blk.size << "\n";
        totalFree += blk.size;
    }
    oss << "空闲总计: " << totalFree << " KB\n\n";

    // 已分配块列表
    oss << "【已分配块列表】\n";
    oss << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)"
        << std::setw(16) << "所属\n";
    oss << std::string(40, '-') << "\n";
    for (const auto& blk : allocBlocks_) {
        oss << std::setw(8) << blk.startAddr << "KB"
            << std::setw(12) << blk.size
            << std::setw(16) << getOwnerLabel(blk.pid) << "\n";
    }

    // ASCII 内存分配图（字符画）
    oss << "\n【内存分配图】\n";
    auto allBlocks = getAllBlocks();
    for (const auto& blk : allBlocks) {
        if (blk.free) {
            oss << "|--free(" << blk.size << "KB)--";
        } else {
            oss << "|##" << getOwnerLabel(blk.pid) << "(" << blk.size << "KB)";
        }
    }
    oss << "|\n\n";

    // 内存块总览（按地址排序统一列出）
    oss << "【内存块总览】\n";
    oss << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)"
        << std::setw(16) << "类型" << "\n";
    oss << std::string(40, '-') << "\n";
    for (const auto& blk : allBlocks) {
        oss << std::setw(8) << blk.startAddr << "KB"
            << std::setw(12) << blk.size
            << std::setw(16);
        if (blk.free) {
            oss << "空闲";
        } else {
            oss << "已分配(" << getOwnerLabel(blk.pid) << ")";
        }
        oss << "\n";
    }

    return oss.str();
}

/** compact — 内存紧缩，合并相邻空闲块 */
void MemoryManager::compact() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    compactInternal();
}

/** compactInternal — 内部紧缩实现（调用者需已持有锁） */
void MemoryManager::compactInternal() {
    if (freeBlocks_.size() < 2) return;

    freeBlocks_.sort([](const MemBlock& a, const MemBlock& b) {
        return a.startAddr < b.startAddr;
    });

    auto it = freeBlocks_.begin();
    auto next = std::next(it);
    while (next != freeBlocks_.end()) {
        if (it->startAddr + it->size == next->startAddr) {
            it->size += next->size;
            next = freeBlocks_.erase(next);
        } else {
            ++it;
            ++next;
        }
    }
}

/**
 * memStat — 内存碎片统计
 *
 * 碎片率 = 1 - (最大连续空闲块 / 总空闲空间) × 100%
 * 如果只有 1 个空闲块，碎片率为 0（没有碎片问题）
 */
std::string MemoryManager::memStat() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    int32_t totalFree = 0;
    for (const auto& blk : freeBlocks_) totalFree += blk.size;

    int32_t totalAlloc = 0;
    for (const auto& blk : allocBlocks_) totalAlloc += blk.size;

    double fragRate = 0.0;
    if (totalFree > 0 && freeBlocks_.size() > 1) {
        int32_t maxFree = 0;
        for (const auto& blk : freeBlocks_) {
            if (blk.size > maxFree) maxFree = blk.size;
        }
        fragRate = (1.0 - (double)maxFree / totalFree) * 100.0;
    }

    std::ostringstream oss;
    oss << "===== 内存统计 =====\n"
        << "总内存:     " << totalSize_ << " KB\n"
        << "已分配:     " << totalAlloc << " KB\n"
        << "空闲:       " << totalFree << " KB\n"
        << "空闲块数:   " << freeBlocks_.size() << "\n"
        << "碎片率:     " << std::fixed << std::setprecision(1) << fragRate << " %\n"
        << "分配算法:   " << getAllocAlgoName() << "\n";
    return oss.str();
}

void MemoryManager::setAllocAlgo(AllocAlgo algo) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    algo_ = algo;
}

std::string MemoryManager::getAllocAlgoName() const {
    switch (algo_) {
        case FIRST_FIT: return "首次适应(FF)";
        case BEST_FIT:  return "最佳适应(BF)";
        case WORST_FIT: return "最坏适应(WF)";
        default:        return "未知";
    }
}

/** getOwnerLabel — 获取内存块所属者的显示标签 */
std::string MemoryManager::getOwnerLabel(int32_t pid) {
    switch (pid) {
        case -1: return "空闲";
        case PID_DATA: return "数据";
        case PID_IO: return "IO";
        case PID_KERNEL: return "内核";
        default: return "PID" + std::to_string(pid);
    }
}

/** getAllBlocks — 合并空闲+已分配块，按地址排序 */
std::list<MemoryManager::MemBlock> MemoryManager::getAllBlocks() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    std::list<MemBlock> result;
    for (const auto& b : freeBlocks_) result.push_back(b);
    for (const auto& b : allocBlocks_) result.push_back(b);

    result.sort([](const MemBlock& a, const MemBlock& b) {
        return a.startAddr < b.startAddr;
    });
    return result;
}

void MemoryManager::setFreeBlocks(const std::list<MemBlock>& blocks) {
    freeBlocks_ = blocks;
}

void MemoryManager::setAllocatedBlocks(const std::list<MemBlock>& blocks) {
    allocBlocks_ = blocks;
}

void MemoryManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    freeBlocks_.clear();
    allocBlocks_.clear();
    freeBlocks_.emplace_back(0, totalSize_, -1, true);
}
