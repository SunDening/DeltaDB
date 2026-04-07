#pragma once

#include "config.h"
#include "status.h"

namespace delta {

/**
 * 负责构建 SST 表文件的核心工具头文件
 * 核心作用：将迭代器中的数据写入到 SST 表文件，用于：
 *  - MemTable Compaction（内存 → 磁盘 L0）
 *  - 层级 Compaction（Ln → Ln+1）
 *
 * 工作流程
┌─────────────────────────────────────────────────────────┐
│  输入：Iterator (合并迭代器)                              │
│  - MemTable 迭代器                                       │
│  - 或多个 SST 文件的合并迭代器                            │
└─────────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────────┐
│  BuildTable()                                           │
│  1. 创建新的 SST 文件 {number}.sst                       │
│  2. 创建 TableBuilder（表构建器）                        │
│  3. 遍历 iter：                                          │
│     - 跳过过期版本（序列号检查）                          │
│     - 删除重复 key（只保留最新版本）                       │
│     - 添加到 TableBuilder                                │
│  4. 关闭 TableBuilder，完成文件                          │
│  5. 填充 FileMetaData                                    │
└─────────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────────┐
│  输出：SST 文件 + FileMetaData                            │
│  - file_number: 文件编号                                 │
│  - file_size: 文件大小                                   │
│  - smallest: 最小 key                                    │
│  - largest: 最大 key                                     │
└─────────────────────────────────────────────────────────┘
 *
 */

struct SSTMetaData;

class Iterator;
class SSTCache;
class VersionEdit;

/**
 * @brief 从 Iterator 读取数据，构建 SSTable 文件
 * @param dbname：数据库路径，用于生成文件名
 * @param sst_cache：sst 表缓存（用于管理打开的表文件）
 * @param iter：数据源迭代器，提供要写入的数据
 * @param sst：输出元数据（文件号、大小、key 范围等）
 */
Status BuildSST(const std::string& dbname, SSTCache* sst_cache, Iterator* iter, SSTMetaData* sst);

// ================================ SSTBuilder class ================================

class BlockBuilder;
class BlockHandle;
class WritableFile;

/**
 * @brief 定义了 SSTable 构建器的公共接口，用于将有序的 key-value 对写入 SSTable 文件
 * TableBuilder
        ↓
    写入有序数据 (Add)
        ↓
    生成 SSTable 文件
        ↓
    包含：数据块 + 索引块 + 元索引块 + Footer
 *
 */
class SSTBuilder {
   private:
    struct Rep;  // 内部实现结构（Pimpl 模式）
    Rep* rep_;   // 指向内部实现的指针

    bool ok() const { return status().ok(); }

    // 写入块到文件（带压缩）
    void WriteBlock(BlockBuilder* block, BlockHandle* handle);

    // 写入原始块到文件（指定压缩类型）
    void WriteRawBlock(const std::string_view& data, CompressionType, BlockHandle* handle);

   public:
    SSTBuilder(WritableFile* file);

    SSTBuilder(const SSTBuilder&) = delete;
    SSTBuilder& operator=(const SSTBuilder&) = delete;

    ~SSTBuilder();

    /**
     * @brief 添加 key-value 到 SSTable
     * 注意：数据先写入缓冲区，满一个块后刷新到文件
     */
    void Add(const std::string_view& key, const std::string_view& value);

    /**
     * @brief 将当前缓冲区的 key-value 对立即写入文件
     */
    void Flush();

    /**
     * @brief 检查错误状态
     */
    Status status() const;

    /**
     * @brief 完成 SSTable 构建
     * 作用：
     *  1. 刷新所有缓冲数据到文件
     *  2. 写入索引块（Index Block）
     *  3. 写入元索引块（Meta Index Block）
     *  4. 写入 Footer （固定 48 字节）
     *  5. 停止使用构造函数传入的文件
     */
    Status Finish();

    /**
     * @brief 放弃当前构建内容，停止使用文件
     */
    void Abandon();

    /**
     * @brief 返回已添加的 entry 数量
     */
    uint64_t EntriesNum() const;

    /**
     * @brief 返回当前文件大小
     * @return 构件中：已写入文件的字节数；Finish()成功后：最终生成的文件大小。
     */
    uint64_t FileSize() const;
};

}  // namespace delta