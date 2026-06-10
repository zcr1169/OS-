// 状态持久化 — 头文件
#pragma once
#include "进程管理器.h"
#include "内存管理器.h"
#include "用户管理器.h"
#include "调度器.h"
#include <string>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <utility>

class StateSerializer {
public:
    static const uint32_t MAGIC = 0x4F535353;
    static const uint32_t VERSION = 1;

    using SwapMap = std::unordered_map<int32_t, std::vector<std::pair<int32_t,int32_t>>>;

    static bool save(const std::string& filePath,
                     const ProcessManager& pm, const MemoryManager& mm,
                     const UserManager& um, const Scheduler& sched,
                     const SwapMap& swapped);
    static bool load(const std::string& filePath,
                     ProcessManager& pm, MemoryManager& mm,
                     UserManager& um, Scheduler& sched,
                     SwapMap& swapped);
    static bool fileExists(const std::string& filePath);

private:
    template <typename T>
    static void writeVal(std::ofstream& out, const T& val) { out.write((const char*)&val, sizeof(T)); }
    template <typename T>
    static void readVal(std::ifstream& in, T& val) { in.read((char*)&val, sizeof(T)); }
    static void writeStr(std::ofstream& out, const std::string& str) {
        int32_t len = (int32_t)str.size();
        writeVal(out, len);
        out.write(str.data(), len);
    }
    static std::string readStr(std::ifstream& in) {
        int32_t len = 0; readVal(in, len);
        if (len < 0 || len > 100000) return "";
        std::string str(len, '\0');
        in.read(&str[0], len);
        return str;
    }
};
