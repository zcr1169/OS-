// ============================================================
// 进程控制块 (PCB — Process Control Block)
//
// 这是操作系统的核心数据结构。每一个进程都对应一个 PCB，
// 里面存了进程的所有信息：ID、状态、优先级、CPU 时间等。
//
// 面试/答辩常问：
//   Q: PCB 里最重要的字段是什么？
//   A: pid（进程唯一标识）、state（当前状态）、priority（调度优先级）
// ============================================================
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

/**
 * PCB — 进程控制块
 *
 * 一个进程的所有信息都记录在这里。
 * 进程管理器（ProcessManager）用一个哈希表（unordered_map<PID, PCB>）
 * 来管理所有 PCB，PID 是键。
 */
struct PCB {
    // ---- 进程状态枚举 ----
    // 一个进程在其生命周期中会经历这些状态转换：
    //
    //   NEW → READY → RUNNING → TERMINATED
    //               ↑    ↓
    //             BLOCKED  SUSPENDED
    //
    // 标准 OS 还有 SUSPENDED_READY / SUSPENDED_BLOCKED 两个子状态，
    // 本模拟器简化为了一个 SUSPENDED。
    enum State : int32_t {
        NEW = 0,         // 新建：PCB 刚创建，还没就绪
        READY = 1,       // 就绪：进程已准备好，在调度队列中等待 CPU
        RUNNING = 2,     // 运行中：正在 CPU 上执行
        BLOCKED = 3,     // 阻塞：等待某事件（如 IO），不在调度队列中
        SUSPENDED = 4,   // 挂起：被手动暂停，不在调度队列中，但保留内存
        TERMINATED = 5   // 终止：进程执行完毕，等待回收
    };

    int32_t pid;                        // 进程 ID（唯一标识，从 1 开始自增）
    int32_t ppid;                       // 父进程 PID（-1 表示无父进程，init 的 ppid=-1）
    std::string name;                   // 进程名（用户指定的名称）
    State state;                        // 当前状态（上面的枚举值之一）
    int32_t priority;                   // 调度优先级（0-15，0 最高，15 最低）
    int32_t cpuTime;                    // 已累计消耗的 CPU 时间（时间片累加）
    int32_t totalMemory;                // 当前占用的总内存（KB）
    int32_t burstTime;                  // 需要执行的总 CPU 时间，跑够自动终止
    std::vector<int32_t> children;      // 子进程 PID 列表（用于级联杀子和 ptree）
    std::string owner;                  // 所属用户（用于用户隔离）

    // ---- 已分配内存块列表 ----
    // 每调用一次 alloc 就会在这里加一条记录
    // pair = (起始地址 KB, 大小 KB)
    // swap_out 时清空，swap_in 时恢复
    std::vector<std::pair<int32_t, int32_t>> memoryBlocks;

    // ---- 默认构造函数 ----
    PCB() : pid(0), ppid(-1), name(""), state(NEW), priority(10),
            cpuTime(0), totalMemory(0), burstTime(5), owner("") {}

    // ---- 带参构造函数 ----
    // burst 默认 5，表示新进程默认需要 5 个 CPU 时间片才能跑完
    PCB(int32_t id, int32_t parent, const std::string& nm, State st,
        int32_t pri, const std::string& own, int32_t burst = 5)
        : pid(id), ppid(parent), name(nm), state(st), priority(pri),
          cpuTime(0), totalMemory(0), burstTime(burst), owner(own) {}

    /**
     * stateToString — 状态转中文显示
     *
     * 在 show_pcb、list_pcb、overview 等地方用到，
     * 这样输出的是中文"就绪"而非英文"READY"。
     */
    static const char* stateToString(State s) {
        switch (s) {
            case NEW:        return "新建";
            case READY:      return "就绪";
            case RUNNING:    return "运行中";
            case BLOCKED:    return "阻塞";
            case SUSPENDED:  return "挂起";
            case TERMINATED: return "终止";
            default:         return "未知";
        }
    }

    /**
     * displayWidth — 计算字符串的终端显示宽度
     *
     * 为什么需要这个函数？
     * 中文在终端里占 2 列宽度，ASCII 占 1 列。
     * 如果直接用 string::size() 做对齐，中文会把表格撑歪。
     * 所以需要这个函数来正确计算"视觉宽度"。
     *
     * 原理：
     *   - 0x00-0x7F：ASCII，占 1 列
     *   - 0x80-0xBF：多字节续字符，跳过
     *   - 0xE0-0xEF：CJK 汉字（UTF-8 3字节编码），占 2 列
     */
    static int displayWidth(const std::string& s) {
        int w = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)      { w += 1; i += 1; }
            else if (c < 0xE0) { w += 1; i += 2; }
            else if (c < 0xF0) { w += 2; i += 3; }
            else               { w += 2; i += 4; }
        }
        return w;
    }

    /** padRight — 右填充到指定显示宽度（中文对齐用） */
    static std::string padRight(const std::string& s, int width) {
        int w = displayWidth(s);
        if (w >= width) return s + " ";
        return s + std::string(width - w, ' ');
    }
};
