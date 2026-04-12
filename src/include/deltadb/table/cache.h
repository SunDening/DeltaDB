#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace delta {

/**
 * 缓存系统的公共接口文件
 *
 * 核心作用：缓存热点数据块（Block），减少磁盘 I/O，提升读写性能。
 * 典型用途：
 *  - 缓存 SST 文件中的数据块（Data Block）
 *  - 缓存过滤器块（Filter Block）
 *  - 缓存索引块（Index Block）
 *
┌─────────────────────────────────────────────────────────┐
│  Cache 是一个抽象接口（纯虚类）                           │
│  - 不指定具体实现                                        │
│  - 线程安全（内部同步）                                   │
│  - 可自动驱逐条目（腾出空间）                             │
│  - 支持按"容量"计费（charge）                            │
└─────────────────────────────────────────────────────────┘
              ↑
              │ 默认实现
              │
┌─────────────────────────────────────────────────────────┐
│  LRU Cache（最近最少使用策略）                            │
│  - NewLRUCache(capacity) 创建                           │
│  - 内置实现，位于 table/cache.cc                        │
└─────────────────────────────────────────────────────────┘
 *
 */

class Cache;

/**
 * @brief 创建 LRU 缓存
 *
 * @param capacity：容量
 */
Cache* NewLRUCache(size_t capacity);

class Cache {
   public:
    Cache() = default;

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    virtual ~Cache();

    /**
     * 缓存条目的引用令牌，类似智能指针的引用计数
     * 持有 Handle 期间，条目不会被驱逐
     *
     * 生命周期：Insert/Lookup → 返回 Handle
                    ↓
                使用 Value() 获取实际值
                    ↓
                Release() → 引用计数 -1，可能触发驱逐
     */
    struct Handle {};

    /**
     * @brief 插入键值对
     * @param Charge 数据大小，容量计费。缓存不限制条目数量，而是限制总容量。
     * @param deleter：删除回调。缓存条目被驱逐时，自动清理资源。
     */
    virtual Handle* Insert(const std::string_view& key, void* value, size_t charge,
                           void (*deleter)(const std::string_view& key, void* value)) = 0;

    /**
     * @brief 查找键。
     * @return 找到：handle；找不到：nullptr
     */
    virtual Handle* Lookup(const std::string_view& key) = 0;

    /**
     * @brief 释放句柄handle
     */
    virtual void Release(Handle* handle) = 0;

    /**
     * @brief 从句柄 handle 获取值
     */
    virtual void* Value(Handle* handle) = 0;

    /**
     * @brief 删除键。
     * 注意：Erase不会立即删除。底层条目将一直保留，直到所有现有的句柄被释放。
     */
    virtual void Erase(const std::string_view& key) = 0;

    /**
     * @brief 生成新 ID
     * 可以由共享相同缓存的多个客户端使用，以对键空间进行分区。
     */
    virtual uint64_t NewId() = 0;

    /**
     * @brief 清理未使用的条目
     */
    virtual void Prune() {}

    /**
     * @brief 返回总占用容量
     */
    virtual size_t TotalCharge() const = 0;
};

}  // namespace delta