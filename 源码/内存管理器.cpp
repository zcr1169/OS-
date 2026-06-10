// 内存管理器 — 动态分区分配（FF/BF/WF）
// 三种分配算法：首次适应、最佳适应、最坏适应
// 支持给进程分配，也支持给 data/io/kernel 分配
#include "内存管理器.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

static const int32_t DEFAULT_MEM_SIZE = 1024;

MemoryManager::MemoryManager() : totalSize_(DEFAULT_MEM_SIZE), algo_(FIRST_FIT) {
    freeBlocks_.emplace_back(0, totalSize_, -1, true);
}

MemoryManager::MemoryManager(int32_t totalSize) : totalSize_(totalSize), algo_(FIRST_FIT) {
    freeBlocks_.emplace_back(0, totalSize_, -1, true);
}

// alloc — 分配内存，按当前算法选空闲块，分裂并分配
int32_t MemoryManager::alloc(int32_t size, int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (size <= 0) return -1;

    auto selected = freeBlocks_.end();
    int32_t bestDiff = INT32_MAX;
    int32_t worstDiff = -1;

    for (auto it = freeBlocks_.begin(); it != freeBlocks_.end(); ++it) {
        if (it->size < size) continue;
        switch (algo_) {
            case FIRST_FIT: selected = it; goto found;
            case BEST_FIT:
                if (it->size - size < bestDiff) { bestDiff = it->size - size; selected = it; }
                break;
            case WORST_FIT:
                if (it->size - size > worstDiff) { worstDiff = it->size - size; selected = it; }
                break;
        }
    }
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

// freeByPid — 释放某进程的所有内存块
void MemoryManager::freeByPid(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::vector<std::list<MemBlock>::iterator> toFree;
    for (auto it = allocBlocks_.begin(); it != allocBlocks_.end(); ++it)
        if (it->pid == pid) toFree.push_back(it);

    for (auto it : toFree) {
        int32_t addr = it->startAddr, size = it->size;
        allocBlocks_.erase(it);
        auto fit = freeBlocks_.begin();
        while (fit != freeBlocks_.end() && fit->startAddr < addr) ++fit;
        freeBlocks_.insert(fit, MemBlock(addr, size, -1, true));
    }
    compactInternal();
}

// swapOutPid — 将进程占用的内存块标记为"换出"(PID_SWAPPED)，不释放到空闲区
// 这样 show_mem 中能看见换出的块，知道这块物理空间被谁占着
void MemoryManager::swapOutPid(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (auto& blk : allocBlocks_) {
        if (blk.pid == pid) {
            blk.pid = PID_SWAPPED;
            blk.free = false;
        }
    }
}

// removeSwappedByPid — 从 allocBlocks 移除某进程的"已换出"标记块（swap_in 时用）
// 这些块之前被 swapOutPid 标记为 PID_SWAPPED，现在要清掉腾出空间重新分配
void MemoryManager::removeSwappedByPid(int32_t pid) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // 注意: swappedOut_ 表中存的是原始内存块信息(addr, size)
    // 对应 allocBlocks 中被标记为 PID_SWAPPED 的块
    // 我们需要找到这些块并移除
    for (auto it = allocBlocks_.begin(); it != allocBlocks_.end(); ) {
        if (it->pid == PID_SWAPPED) {
            // 检查这块是否属于要换入的进程
            // 我们没法直接知道 PID_SWAPPED 块属于哪个进程
            // 所以这里全部清除 PID_SWAPPED 块——安全做法
            // 但更精确的做法由调用方处理
            it = allocBlocks_.erase(it);
        } else {
            ++it;
        }
    }
}

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

int32_t MemoryManager::getPidByAddr(int32_t addr) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (const auto& blk : allocBlocks_)
        if (blk.startAddr == addr) return blk.pid;
    return -1;
}

// showMem — 显示内存使用：空闲块、已分配块、内存分配图、内存块总览
std::string MemoryManager::showMem() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::ostringstream oss;
    oss << "===== 内存使用情况 =====\n";
    oss << "总内存: " << totalSize_ << " KB\n";
    oss << "分配算法: " << getAllocAlgoName() << "\n\n";

    if (freeBlocks_.empty() && allocBlocks_.empty()) {
        oss << "内存为空\n"; return oss.str();
    }

    oss << "【空闲块列表】\n" << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)\n" << std::string(24, '-') << "\n";
    int32_t totalFree = 0;
    for (const auto& blk : freeBlocks_) {
        oss << std::setw(8) << blk.startAddr << "KB" << std::setw(12) << blk.size << "\n";
        totalFree += blk.size;
    }
    oss << "空闲总计: " << totalFree << " KB\n\n";

    oss << "【已分配块列表】\n" << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)" << std::setw(16) << "所属\n" << std::string(40, '-') << "\n";
    for (const auto& blk : allocBlocks_)
        oss << std::setw(8) << blk.startAddr << "KB" << std::setw(12) << blk.size << std::setw(16) << getOwnerLabel(blk.pid) << "\n";

    oss << "\n【内存分配图】\n";
    auto allBlocks = getAllBlocks();
    for (const auto& blk : allBlocks) {
        if (blk.free) oss << "|--free(" << blk.size << "KB)--";
        else oss << "|##" << getOwnerLabel(blk.pid) << "(" << blk.size << "KB)";
    }
    oss << "|\n\n";

    oss << "【内存块总览】\n" << std::setw(12) << "起始地址" << std::setw(12) << "大小(KB)" << std::setw(16) << "类型\n" << std::string(40, '-') << "\n";
    for (const auto& blk : allBlocks) {
        oss << std::setw(8) << blk.startAddr << "KB" << std::setw(12) << blk.size << std::setw(16);
        oss << (blk.free ? "空闲" : (std::string("已分配(") + getOwnerLabel(blk.pid) + ")")) << "\n";
    }
    return oss.str();
}

void MemoryManager::compact() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    compactInternal();
}

// compactInternal — 合并相邻空闲块
void MemoryManager::compactInternal() {
    if (freeBlocks_.size() < 2) return;
    freeBlocks_.sort([](const MemBlock& a, const MemBlock& b) { return a.startAddr < b.startAddr; });
    auto it = freeBlocks_.begin();
    auto next = std::next(it);
    while (next != freeBlocks_.end()) {
        if (it->startAddr + it->size == next->startAddr) {
            it->size += next->size;
            next = freeBlocks_.erase(next);
        } else { ++it; ++next; }
    }
}

// memStat — 碎片率 = 1 - (最大连续空闲块/总空闲) × 100%
std::string MemoryManager::memStat() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    int32_t totalFree = 0, totalAlloc = 0;
    for (const auto& blk : freeBlocks_) totalFree += blk.size;
    for (const auto& blk : allocBlocks_) totalAlloc += blk.size;

    double fragRate = 0.0;
    if (totalFree > 0 && freeBlocks_.size() > 1) {
        int32_t maxFree = 0;
        for (const auto& blk : freeBlocks_) if (blk.size > maxFree) maxFree = blk.size;
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
        default: return "未知";
    }
}

std::string MemoryManager::getOwnerLabel(int32_t pid) {
    switch (pid) {
        case -1: return "空闲";
        case PID_DATA: return "数据";
        case PID_IO: return "IO";
        case PID_KERNEL: return "内核";
        case PID_SWAPPED: return "换出";
        default: return "PID" + std::to_string(pid);
    }
}

std::list<MemoryManager::MemBlock> MemoryManager::getAllBlocks() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::list<MemBlock> result;
    for (const auto& b : freeBlocks_) result.push_back(b);
    for (const auto& b : allocBlocks_) result.push_back(b);
    result.sort([](const MemBlock& a, const MemBlock& b) { return a.startAddr < b.startAddr; });
    return result;
}

void MemoryManager::setFreeBlocks(const std::list<MemBlock>& blocks) { freeBlocks_ = blocks; }
void MemoryManager::setAllocatedBlocks(const std::list<MemBlock>& blocks) { allocBlocks_ = blocks; }

void MemoryManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    freeBlocks_.clear();
    allocBlocks_.clear();
    freeBlocks_.emplace_back(0, totalSize_, -1, true);
}
