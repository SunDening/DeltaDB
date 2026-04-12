#pragma once

#include <string>
#include <string_view>

namespace delta {

class Comparator {
   public:
    virtual ~Comparator();

    /**
     * 三路比较：
     * - <0 表示 a < b
     * - ==0 表示 a == b
     * - >0 表示 a > b
     */
    virtual int Compare(const std::string_view& a, const std::string_view& b) const = 0;

    // 比较器名称，用于检查兼容性
    virtual const char* Name() const = 0;

    /**
     * 高级功能：用于压缩索引块
     * 该函数用于在保证不超出上限（limit）的前提下，尝试将其实键（start）缩短为一个更短的、能与原键区分的内部表示形式，以节省索引块等内部数据结构的存储空间
     */
    virtual void FindShortestSeparator(std::string* start, const std::string_view& limit) const = 0;

    /**
     * 用于将输入的key修改为一个大于原键的最短字符串，
     * 若无法找到更短的后继键则保持原值不变，是一种用于优化比较器性能的虚函数接口
     */
    virtual void FindShortSuccessor(std::string* key) const = 0;
};

const Comparator* BytewiseComparator();

}  // namespace delta