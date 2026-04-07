#pragma once

#include "status.h"

namespace delta {

/**
 * WriteBatch 允许将多个写操作（Put/Delete）打包成一个批次，然后原子性地应用到数据库中，要么全部成功，要么全部失败
 */
class WriteBatch {
   public:
    class Handler {
       public:
        virtual ~Handler();
        virtual void Put(const std::string_view& key, const std::string_view& value) = 0;
        virtual void Delete(const std::string_view& key) = 0;
    };

    WriteBatch();

    WriteBatch(const WriteBatch&) = delete;
    WriteBatch& operator=(const WriteBatch&) = default;

    ~WriteBatch();

    // 添加键值对写入操作
    void Put(const std::string_view& key, const std::string_view& value);

    // 添加删除操作
    void Delete(const std::string_view& key);

    // 清空批次中地所有操作
    void Clear();

    // 返回批次引起地数据库变化近似大小
    size_t ApproximateSize() const;

    // 将另一个批次的操作追加到当前批次
    void Append(const WriteBatch& source);

    // 使用 Handler 遍历所有操作
    Status Iterate(Handler* handler) const;

   private:
    friend class WriteBatchInternal;  // 允许该类访问 write_batch 的私有成员

    // 所有操作被序列化成二进制存储在 rep_ 中，具体格式在 write_batch.cc 中定义
    std::string rep_;  // 存储序列化后的操作序列
};

}  // namespace delta