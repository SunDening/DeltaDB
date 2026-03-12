#pragma once

#include <string>

class SnappyCompressor {
public:
    // 压缩：输入原始字符串，输出压缩字符串
    static bool compress(const std::string& input, std::string& output);

    // 解压：输入压缩字符串，输出解压结果
    static bool decompress(const std::string& input, std::string& output);
};
