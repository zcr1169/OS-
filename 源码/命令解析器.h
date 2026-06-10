// 命令解析器 — 空格分割输入为命令名和参数
#pragma once
#include <string>
#include <vector>
#include <sstream>

class CommandParser {
public:
    struct Command {
        std::string name;
        std::vector<std::string> args;
        std::string raw;
    };

    static Command parse(const std::string& input) {
        Command cmd;
        cmd.raw = input;
        std::istringstream iss(input);
        std::string token;
        if (iss >> token) cmd.name = toLower(token);
        while (iss >> token) cmd.args.push_back(token);
        return cmd;
    }

    static std::string toLower(const std::string& str) {
        std::string r = str;
        for (char& c : r) c = (char)tolower((unsigned char)c);
        return r;
    }

    static bool toInt(const std::string& str, int32_t& out) {
        try { size_t pos; out = std::stoi(str, &pos); return pos == str.size(); }
        catch (...) { return false; }
    }
};
