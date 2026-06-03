#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

/**
 * 进程控制块 (Process Control Block)
 */
struct PCB {
    enum State : int32_t {
        NEW = 0,
        READY = 1,
        RUNNING = 2,
        BLOCKED = 3,
        SUSPENDED = 4,
        TERMINATED = 5
    };

    int32_t pid;
    int32_t ppid;           // -1表示无父进程(init)
    std::string name;
    State state;
    int32_t priority;       // 0-15, 0最高
    int32_t cpuTime;        // 累计CPU时间片
    int32_t totalMemory;    // 占用总内存(KB)
    int32_t burstTime;      // 需要执行的总CPU时间，跑够自动终止
    std::vector<int32_t> children;
    std::string owner;

    // 已分配内存块列表: (起始地址KB, 大小KB)
    std::vector<std::pair<int32_t, int32_t>> memoryBlocks;

    PCB() : pid(0), ppid(-1), name(""), state(NEW), priority(10),
            cpuTime(0), totalMemory(0), burstTime(5), owner("") {}

    PCB(int32_t id, int32_t parent, const std::string& nm, State st,
        int32_t pri, const std::string& own, int32_t burst = 5)
        : pid(id), ppid(parent), name(nm), state(st), priority(pri),
          cpuTime(0), totalMemory(0), burstTime(burst), owner(own) {}

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

    /** 计算字符串的终端显示宽度(中文占2列, ASCII占1列) */
    static int displayWidth(const std::string& s) {
        int w = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)      { w += 1; i += 1; }  // ASCII
            else if (c < 0xE0) { w += 1; i += 2; }  // 拉丁扩展
            else if (c < 0xF0) { w += 2; i += 3; }  // CJK等全角
            else               { w += 2; i += 4; }  // emoji等
        }
        return w;
    }

    /** 右填充到指定显示宽度 */
    static std::string padRight(const std::string& s, int width) {
        int w = displayWidth(s);
        if (w >= width) return s + " ";
        return s + std::string(width - w, ' ');
    }
};
