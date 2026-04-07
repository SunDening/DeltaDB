#pragma once

#include <cstdint>
#include <string>

#include "cache.h"
#include "dbformat.h"
#include "table.h"

namespace delta {

/**
 * SSTCache 是用于缓存 SSTable 文件对象的组件，位于 Cache（通用缓存）和 Table（SSTable文件）之间。
 *
 * 设计目的
    ┌─────────────────────────────────────────────────────────┐
    │                    VersionSet                           │
    │  (管理多个 Version，每个 Version 包含多个 SSTable 文件)    │
    └─────────────────────────────────────────────────────────┘
                              ↓
    ┌─────────────────────────────────────────────────────────┐
    │                    TableCache                           │
    │  (缓存打开的 Table 对象，避免重复打开文件)                 │
    │  - 基于文件号 (file_number) 索引                         │
    │  - 使用 LRU 策略管理缓存                                  │
    │  - 线程安全                                              │
    └─────────────────────────────────────────────────────────┘
                              ↓
    ┌─────────────────────────────────────────────────────────┐
    │                      Cache                              │
    │  (通用缓存实现，通常是 ShardedCache)                      │
    └─────────────────────────────────────────────────────────┘
 *
 * 问题背景：
 *  LevelDB 可能有成百上千个 SSTable 文件，每次读取都打开/关闭文件会导致：
 *      - 频繁的系统调用开销
 *      - 无法复用已加载的数据块缓存
 * SSTCache 的解决方案：
 *  - 缓存键构造：将 file_number 编码为字符串
 *  - 缓存值：Table* 对象
    // 示例：访问 file_number = 123 的 SSTable
    Table* table;
    auto* iter = table_cache->NewIterator(options, 123, file_size, &table);
    // 如果 file 123 已在缓存，直接使用
    // 如果不在缓存，打开文件 → 创建 Table → 加入缓存 → 使用

    // 使用完毕后，迭代器销毁时会自动减少缓存引用计数
    delete iter;
 *
 */

class SSTCache {
   private:
    const std::string dbname_;
    Cache* cache_;  // 底层通用 cache

    /**
     * @brief 查找或打开指定 SSTable 文件，返回缓存句柄
     * @param file_number：文件编号（唯一标识）
     * @param file_size：文件大小
     * @param handle：输出的缓存句柄指针
     *
     * 1. 如果文件已在缓存中，返回缓存句柄; 2. 如果不在缓存中，打开文件并加入缓存;
     */
    Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle);

   public:
    /**
     * @param dbname：数据库目录名
     * @param entries_num：缓存的 SSTable 文件数量上限
     */
    SSTCache(const std::string& dbname, int entries_num);

    SSTCache(const SSTCache&) = delete;
    SSTCache& operator=(const SSTCache&) = delete;

    ~SSTCache();

    /**
     * @brief 为指定 SSTable 文件创建迭代器
     * @param file_number：文件编号
     * @param file_size：文件大小
     * @param table_ptr：可选，输出 Table 指针
     * - 如果文件已在缓存中，直接复用
     * - 如果不在缓存中，打开文件并加入缓存
     * - table_ptr 可用于获取底层的 Table 对象（由缓存管理，不应手动删除）
     */
    Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                          Table** table_ptr = nullptr);

    /**
     * @brief 在指定 SSTable 文件中查找键 k
     * @param options：读取选项
     * @param file_number：文件编号
     * @param file_size：文件大小
     * @param k：查询键
     * @param arg：结果处理函数的参数
     * @param handle_result：回调函数，处理找到的键值对
     */
    Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const std::string_view& k,
               void* arg, void (*handle_result)(void*, const std::string_view&, const std::string_view&));

    /**
     * @brief 从缓存中移除指定文件号的 SSTable
     */
    void Evict(uint64_t file_number);
};

}  // namespace delta