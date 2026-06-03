#pragma once
#include <string>
#include <vector>
#include <sstream>

/**
 * 命令解析器
 * 负责将用户输入文本解析为命令名和参数列表
 */
class CommandParser {
public:
    /** 解析后的命令结构 */
    struct Command {
        std::string name;                // 命令名(小写)
        std::vector<std::string> args;   // 参数列表
        std::string raw;                 // 原始输入

        Command() = default;
        Command(const std::string& n, const std::vector<std::string>& a, const std::string& r)
            : name(n), args(a), raw(r) {}
    };

    /** 解析用户输入 */
    static Command parse(const std::string& input) {
        Command cmd;
        cmd.raw = input;

        std::istringstream iss(input);
        std::string token;

        // 第一个token为命令名
        if (iss >> token) {
            cmd.name = toLower(token);
        }

        // 剩余为参数
        while (iss >> token) {
            cmd.args.push_back(token);
        }

        return cmd;
    }

    /** 将字符串转为小写 */
    static std::string toLower(const std::string& str) {
        std::string result = str;
        for (char& c : result) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    /** 尝试将字符串转为整数, 失败返回false */
    static bool toInt(const std::string& str, int32_t& out) {
        try {
            size_t pos;
            out = std::stoi(str, &pos);
            return pos == str.size();
        } catch (...) {
            return false;
        }
    }

    /** 字符串分割(备用) */
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
