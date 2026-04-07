#pragma once

#include <cstdint>

#include "iterator.h"
#include "log.h"

/**
 * 最重要的公共头文件，定义了数据库的核心抽象接口
 */
namespace delta {

// 版本信息
static const int kMajorVersion = 1;
static const int kMinorVersion = 23;

class WriteBatch;

/**
 * 快照
 * 作用：数据库状态的不可变视图
 * 特性：多线程安全，无需外部同步
 * 用途 ：一致性读，避免读写冲突
 */
class Snapshot {
   protected:
    virtual ~Snapshot();
};

/**
 * 范围
 */
struct Range {
    std::string_view start;  // 范围起始（包含）
    std::string_view limit;  // 范围结束（不包含）

    Range() = default;
    Range(const std::string_view& s, const std::string_view& l) : start(s), limit(l) {}
};

/**
 * DB 核心接口
 */
class DB {
   public:
    /**
     * @brief 使用指定的“name”打开数据库。是外部使用 delta db 的入口点
     * 在*dbptr中存储指向堆分配数据库的指针并在成功时返回 OK
     * 在*dbptr中存储nullptr并在错误时返回非ok状态。
     * 当不再需要*dbptr时，调用者应该删除它。
     * 注意：这里去掉了 option 参数，改用全局配置 gDBConfig
     */
    static Status Open(const std::string& name, DB** dbptr);

    DB() = default;

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    virtual ~DB();

    // 写入键值对
    virtual Status Put(const WriteOptions& options, const std::string_view& key, const std::string_view& value) = 0;

    // 删除键
    virtual Status Delete(const WriteOptions& options, const std::string_view& key) = 0;

    // 原子批量写入
    virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

    // 单条读取
    virtual Status Get(const ReadOptions& options, const std::string_view& key, std::string* value) = 0;

    /**
     * 返回数据库内容的堆分配迭代器。
     * NewIterator（）的结果最初是无效的（调用者在使用迭代器之前必须调用其中一个Seek方法）。
     */
    virtual Iterator* NewIterator(const ReadOptions& options) = 0;

    // 获取快照
    virtual const Snapshot* GetSnapshot() = 0;

    // 释放快照
    virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

    /**
     * 数据库属性与统计
     * 支持的属性：
     *  - leveldb.num-files-at-level<N> —— 指定层级的 SST 文件数量
     *  - leveldb.stats —— 内部运行统计信息（多行字符串）
     *  - leveldb.sstables —— 所有 SST 表的详细描述
     *  - leveldb.approximate-memory-usage —— 近似内存使用量（字节）
     */
    virtual bool GetProperty(const std::string_view& property, std::string* value) = 0;

    // 估算指定键范围占用的磁盘空间
    virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) = 0;

    /**
     * 手动压缩
     *  - 合并指定范围的数据
     *  - 删除过期版本和已删除数据
     *  - 重新排列数据以减少访问开销
     */
    virtual void CompactRange(const std::string_view* begin, const std::string_view* end) = 0;
};

// 销毁数据库（删除所有数据）
Status DestroyDB(const std::string& name);

// 修复数据库（尽可能恢复数据）
Status RepairDB(const std::string& dbname);

}  // namespace delta
