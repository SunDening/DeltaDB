#pragma once

#include <string>

namespace delta {

/**
核心功能：
    数据库可以配置一个自定义的 FilterPolicy 对象。
    它的主要职责是从一组键（keys）中生成一个小的“过滤器”（filter）。
    这些过滤器会被存储在 LevelDB 内部。
工作原理：
    当执行 DB::Get() 查询操作时，LevelDB 会自动咨询这些过滤器。
    过滤器的作用是快速判断某个键一定不存在于对应的文件中。
    如果过滤器判断键不存在，LevelDB 就会跳过对该文件的磁盘读取。
性能收益：
    通过这种机制，可以将每次查询可能涉及的多次磁盘随机读取（disk
seeks），大幅减少到可能只需要一次，从而显著提高读取性能。 使用建议：
    注释明确指出，大多数用户不需要自己实现，直接使用内置的布隆过滤器（Bloom Filter）支持即可，也就是通过
NewBloomFilterPolicy() 函数来创建。
 */

class FilterPolicy {
   public:
    virtual ~FilterPolicy() = default;

    virtual const char* Name() const = 0;

    virtual void CreateFilter(const std::string_view* keys, int n, std::string* dst) const = 0;

    virtual bool KeyMayMatch(const std::string_view& key, const std::string_view& filter) const = 0;
};

}  // namespace delta