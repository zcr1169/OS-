// ============================================================
// 命令解析器 — 将用户输入的字符串解析为命令名和参数
//
// 原理：空格分割字符串，第一个 token 是命令名（转小写），
// 其余 token 是参数列表。
//
// 示例：输入 "Create_PCB test 5 1 10"
//       → name = "create_pcb"
//       → args = ["test", "5", "1", "10"]
// ============================================================
#pragma once
#include <string>
#include <vector>
#include <sstream>

class CommandParser {
public:
    /** 解析后的命令结构 */
    struct Command {
        std::string name;                // 命令名（全小写）
        std::vector<std::string> args;   // 参数列表
        std::string raw;                 // 原始输入文本

        Command() = default;
        Command(const std::string& n, const std::vector<std::string>& a, const std::string& r)
            : name(n), args(a), raw(r) {}
    };

    /**
     * parse — 解析用户输入
     *
     * 用 istringstream 按空格分割，
     * 第一个 token 转小写作为命令名，
     * 其余 token 作为参数列表。
     */
    static Command parse(const std::string& input) {
        Command cmd;
        cmd.raw = input;

        std::istringstream iss(input);
        std::string token;

        if (iss >> token) {
            cmd.name = toLower(token);
        }

        while (iss >> token) {
            cmd.args.push_back(token);
        }

        return cmd;
    }

    /** toLower — 字符串转小写 */
    static std::string toLower(const std::string& str) {
        std::string result = str;
        for (char& c : result) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    /** toInt — 字符串转整数，失败返回 false */
    static bool toInt(const std::string& str, int32_t& out) {
        try {
            size_t pos;
            out = std::stoi(str, &pos);
            return pos == str.size();
        } catch (...) {
            return false;
        }
    }

    /** split — 字符串分割（备用） */
    static std::vector<std::string> split(const std::string& str, char delim) {
        std::vector<std::string> result;
        std::istringstream iss(str);
        std::string token;
        while (std::getline(iss, token, delim)) {
            if (!token.empty()) result.push_back(token);
        }
        return result;
    }
};
