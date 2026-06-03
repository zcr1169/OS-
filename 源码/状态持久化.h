#pragma once
#include "进程控制块.h"
#include "进程管理器.h"
#include "内存管理器.h"
#include "用户管理器.h"
#include "调度器.h"
#include <string>
#include <fstream>

// 状态持久化管理器
// 保存/恢复系统状态到二进制文件
class StateSerializer {
public:
    // 保存系统状态到文件
    static bool save(const std::string& filePath,
                     const ProcessManager& pm,
                     const MemoryManager& mm,
                     const UserManager& um,
                     const Scheduler& sched);

    // 从文件加载系统状态
    static bool load(const std::string& filePath,
                     ProcessManager& pm,
                     MemoryManager& mm,
                     UserManager& um,
                     Scheduler& sched);

    // 检查状态文件是否存在
    static bool fileExists(const std::string& filePath);

private:
    // 魔数 "OSSS"
    static const uint32_t MAGIC = 0x4F535353;
    static const uint32_t VERSION = 1;

    // 写入/读取 基本类型
    template<typename T>
    static void writeVal(std::ofstream& out, const T& val) {
        out.write(reinterpret_cast<const char*>(&val), sizeof(T));
    }

    // 写入字符串(长度前缀)
    static void writeStr(std::ofstream& out, const std::string& str) {
        int32_t len = static_cast<int32_t>(str.size());
        writeVal(out, len);
        if (len > 0) out.write(str.data(), len);
    }

    // 读取基本类型
    template<typename T>
    static void readVal(std::ifstream& in, T& val) {
        in.read(reinterpret_cast<char*>(&val), sizeof(T));
    }

    // 读取字符串
    static std::string readStr(std::ifstream& in) {
        int32_t len = 0;
        readVal(in, len);
        if (len <= 0 || len > 1024) {
            in.setstate(std::ios::failbit);  // 标记错误, 防止后续读错位
            return "";
        }
        std::string str(len, '\0');
        in.read(&str[0], len);
        return str;
    }
};
