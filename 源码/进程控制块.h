// PCB — 进程控制块，存一个进程的所有信息
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

struct PCB {
    enum State : int32_t {
        NEW = 0, READY = 1, RUNNING = 2,
        BLOCKED = 3, SUSPENDED = 4, TERMINATED = 5
    };

    int32_t pid;                                   // 进程 ID
    int32_t ppid;                                  // 父进程 PID，-1 表示无父
    std::string name;                              // 进程名
    State state;                                   // 当前状态
    int32_t priority;                              // 0-15，0最高
    int32_t cpuTime;                               // 已消耗 CPU 时间
    int32_t totalMemory;                           // 占用内存 KB
    int32_t burstTime;                             // 需要总 CPU 时间，跑够终止
    std::vector<int32_t> children;                 // 子进程 PID 列表
    std::string owner;                             // 所属用户
    std::vector<std::pair<int32_t, int32_t>> memoryBlocks; // 内存块列表

    PCB() : pid(0), ppid(-1), name(""), state(NEW), priority(10),
            cpuTime(0), totalMemory(0), burstTime(5), owner("") {}

    PCB(int32_t id, int32_t parent, const std::string& nm, State st,
        int32_t pri, const std::string& own, int32_t burst = 5)
        : pid(id), ppid(parent), name(nm), state(st), priority(pri),
          cpuTime(0), totalMemory(0), burstTime(burst), owner(own) {}

    // 状态转中文
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

    // 计算字符串在终端中的显示宽度（中文占2，英文占1）
    // 用于 list_pcb 表格对齐
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

    // 右填充到指定宽度（中文对齐用）
    static std::string padRight(const std::string& s, int width) {
        int w = displayWidth(s);
        if (w >= width) return s + " ";
        return s + std::string(width - w, ' ');
    }
};
