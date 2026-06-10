// ============================================================
// 状态持久化 — 头文件声明
//
// 模板函数 writeVal/readVal 用于序列化基本类型，
// writeStr/readStr 用于序列化字符串。
// ============================================================
#pragma once
#include "进程管理器.h"
#include "内存管理器.h"
#include "用户管理器.h"
#include "调度器.h"
#include <string>
#include <fstream>

class StateSerializer {
public:
    static const uint32_t MAGIC = 0x4F535353;   // "OSS" 魔数
    static const uint32_t VERSION = 1;

    static bool save(const std::string& filePath,
                     const ProcessManager& pm,
                     const MemoryManager& mm,
                     const UserManager& um,
                     const Scheduler& sched);

    static bool load(const std::string& filePath,
                     ProcessManager& pm,
                     MemoryManager& mm,
                     UserManager& um,
                     Scheduler& sched);

    static bool fileExists(const std::string& filePath);

private:
    /** 写入基本类型（直接写内存字节） */
    template <typename T>
    static void writeVal(std::ofstream& out, const T& val) {
        out.write(reinterpret_cast<const char*>(&val), sizeof(T));
    }

    /** 读取基本类型 */
    template <typename T>
    static void readVal(std::ifstream& in, T& val) {
        in.read(reinterpret_cast<char*>(&val), sizeof(T));
    }

    /** 写入字符串（先写 int32 长度，再写内容） */
    static void writeStr(std::ofstream& out, const std::string& str) {
        int32_t len = static_cast<int32_t>(str.size());
        writeVal(out, len);
        out.write(str.data(), len);
    }

    /** 读取字符串（先读 int32 长度，再读内容） */
    static std::string readStr(std::ifstream& in) {
        int32_t len = 0;
        readVal(in, len);
        if (len < 0 || len > 100000) return "";
        std::string str(len, '\0');
        in.read(&str[0], len);
        return str;
    }
};
