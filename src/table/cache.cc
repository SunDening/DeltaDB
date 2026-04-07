#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "cache.h"
#include "util.h"

namespace delta {

/**
 * LRU 实现原理：
    LRU Cache 数据结构：
    ┌─────────────────────────────────────────────────────────┐
    │  Hash Table (快速查找)                                   │
    │  key → CacheEntry*                                      │
    └─────────────────────────────────────────────────────────┘
                  ↓
    ┌─────────────────────────────────────────────────────────┐
    │  双向链表 (LRU 队列)                                     │
    │  head ←→ [最近使用] ←→ [较少使用] ←→ [最久未用] ←→ tail │
    └─────────────────────────────────────────────────────────┘

    插入/访问：移动到 head
    驱逐：从 tail 删除
 *
 * 类结构：
    Cache (抽象基类，头文件定义)
        ↑
        │ 继承
        │
    ShardedLRUCache (对外暴露的实现类)
        │
        ├── 由 16 个 LRUCache 分片组成（kNumShards = 16）
        │   目的：减少锁竞争，提高并发性能
        │
        └── 每个 LRUCache 包含:
            ├── HandleTable (哈希表，快速查找)
            └── 双向链表 (LRU 队列)
 *
 * 为什么用分片？
    单 LRUCache:
    ┌─────────────────────────────────────┐
    │  Mutex → 所有操作争用同一把锁        │
    └─────────────────────────────────────┘

    ShardedLRUCache (16 分片):
    ┌─────┬─────┬─────┬─────┬ ... ┬─────┐
    │Shard0│Shard1│Shard2│Shard3│ ... │Shard15│
    │ Lock │ Lock │ Lock │ Lock │     │ Lock  │
    └─────┴─────┴─────┴─────┴ ... ┴─────┘
            ↓
    并发度提升约 16 倍（理想情况）
 *
 * 调用关系：
    NewLRUCache(capacity)
        ↓
    创建 ShardedLRUCache
        ↓
    容量均分到 16 个 shard

    Insert/Lookup/Release/Erase
        ↓
    Hash(key) → 选择
 *
 */

Cache::~Cache() {}

namespace {

// ============================================================================
// LRUHandle - 缓存条目结构
// ============================================================================

/**
 * @brief 缓存条目的数据结构（变长堆分配）
 *
 * 内存布局：
 * +------------------+
 * | LRUHandle 头部   |
 * +------------------+
 * | key_data[0]      |  ← 键数据开始（变长）
 * | key_data[1]      |
 * | ...              |
 * +------------------+
 *
 * 条目保存在循环双向链表中，按访问时间排序
 */
struct LRUHandle {
    void* value;                                            // 缓存的值
    void (*deleter)(const std::string_view&, void* value);  // 删除回调

    LRUHandle* next_hash;  // 哈希表冲突链表下一节点
    LRUHandle* next;       // LRU 链表下一节点
    LRUHandle* prev;       // LRU 链表上一节点

    size_t charge;      // 容量计费（通常是数据大小）
    size_t key_length;  // 键长度
    bool in_cache;      // 是否在缓存中（被缓存引用）
    uint32_t refs;      // 引用计数（包括缓存引用）
    uint32_t hash;      // 键的哈希值（用于快速比较）
    char key_data[1];   // 键数据的起始位置（变长数组）

    /**
     * @brief 获取键的 Slice 视图
     * @return Slice 键
     *
     * 注意：只有空列表头的 next == this，此时 key() 无意义
     */
    std::string_view key() const {
        // 空双向链表仅有头节点，头节点不存储 key
        assert(next != this);
        return std::string_view(key_data, key_length);
    }
};

// ============================================================================
// HandleTable - 哈希表实现
// ============================================================================

/**
 * @brief 简单的哈希表实现
 *
 * 设计理由：
 * 1. 避免移植性问题
 * 2. 比某些编译器内置哈希表更快
 *    （测试显示 readrandom 性能提升约 5%）
 *
 * 数据结构：
 * - 桶数组：list_[0..length_-1]
 * - 每个桶是一个链表，存储哈希冲突的条目
 *
 * 负载因子控制：
 * - 目标：平均链表长度 <= 1
 * - 当 elems_ > length_ 时，扩容 2 倍
 */
class HandleTable {
   private:
    // 哈希表由桶数组组成，每个桶是一个链表
    uint32_t length_;   // 桶数组大小（2 的幂）
    uint32_t elems_;    // 当前元素数量
    LRUHandle** list_;  // 桶数组指针

    /**
     * @brief 找到键对应的桶位置
     * @param key 键
     * @param hash 键的哈希值
     * @return LRUHandle** 指向桶或链表节点的指针
     *
     * 返回值是指向指针的指针，方便插入/删除操作
     */
    LRUHandle** FindPointer(const std::string_view& key, uint32_t hash) {
        // 计算桶索引（hash & (length_-1) 等价于 hash % length_）
        LRUHandle** ptr = &list_[hash & (length_ - 1)];

        // 遍历链表，查找匹配的键
        while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
            ptr = &(*ptr)->next_hash;
        }
        return ptr;  // 返回找到的位置（或链表尾部）
    }

    /**
     * @brief 扩容哈希表
     *
     * 扩容策略：
     * 1. 找到最小的 2 的幂，使得 new_length >= elems_
     * 2. 重新哈希所有条目
     *
     * 时间复杂度：O(n)
     */
    void Resize() {
        uint32_t new_length = 4;
        while (new_length < elems_) {
            new_length *= 2;
        }

        // 分配新桶数组
        LRUHandle** new_list = new LRUHandle*[new_length];
        memset(new_list, 0, sizeof(new_list[0]) * new_length);

        // 重新哈希所有条目
        uint32_t count = 0;
        for (uint32_t i = 0; i < length_; i++) {
            LRUHandle* h = list_[i];
            while (h != nullptr) {
                LRUHandle* next = h->next_hash;
                uint32_t hash = h->hash;

                // 计算新桶位置并插入
                LRUHandle** ptr = &new_list[hash & (new_length - 1)];
                h->next_hash = *ptr;
                *ptr = h;
                h = next;
                count++;
            }
        }
        assert(elems_ == count);
        // 释放旧数组
        delete[] list_;
        list_ = new_list;
        length_ = new_length;
    }

   public:
    HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }

    ~HandleTable() { delete[] list_; }

    /**
     * @brief 查找键对应的条目
     * @param key 键
     * @param hash 键的哈希值
     * @return LRUHandle* 找到的条目，未找到返回 nullptr
     */
    LRUHandle* Lookup(const std::string_view& key, uint32_t hash) { return *FindPointer(key, hash); }

    /**
     * @brief 插入条目到哈希表
     * @param h：要插入的条目
     * @return LRUHandle*：被替换的旧条目（如果有）
     *
     * 如果键已存在，替换旧条目并返回旧条目
     * 如果键不存在，直接插入并返回 nullptr
     */
    LRUHandle* Insert(LRUHandle* h) {
        // 找到对应的桶位置
        LRUHandle** ptr = FindPointer(h->key(), h->hash);
        LRUHandle* old = *ptr;

        // 将 h 插入到链表头部
        h->next_hash = (old == nullptr ? nullptr : old->next_hash);
        *ptr = h;
        if (old == nullptr) {
            elems_++;
            if (elems_ > length_) {
                Resize();
            }
        }
        return old;
    }

    /**
     * @brief 从哈希表删除条目
     * @param key 键
     * @param hash 键的哈希值
     * @return LRUHandle* 被删除的条目，未找到返回 nullptr
     */
    LRUHandle* Remove(const std::string_view& key, uint32_t hash) {
        LRUHandle** ptr = FindPointer(key, hash);
        LRUHandle* result = *ptr;
        if (result != nullptr) {
            *ptr = result->next_hash;
            --elems_;
        }
        return result;
    }
};

// ============================================================================
// LRUCache - 单分片 LRU 缓存
// ============================================================================

/**
 * @brief 单分片的 LRU 缓存实现
 *
 * 数据结构：
 * - 哈希表：快速查找
 * - 双向链表：维护 LRU 顺序
 * - 互斥锁：线程安全
 *
 * 两个链表：
 * - lru_：未使用的条目（LRU 顺序）
 * - in_use_：正在使用的条目（无特定顺序）
 */
class LRUCache {
   private:
    size_t capacity_;  // 缓存容量
    mutable std::mutex mtx_;
    size_t usage_;  // 当前使用量
    // LRU 链表头（哑元节点）
    // lru_.prev 是最新条目，lru_.next 是最旧条目
    // 条目特征：refs==1 && in_cache==true
    LRUHandle lru_;
    // 使用中链表头（哑元节点）
    // 条目特征：refs>=2 && in_cache==true
    LRUHandle in_use_;
    HandleTable table_;  // 哈希表

    void LRU_Remove(LRUHandle* e);
    void LRU_Append(LRUHandle* list, LRUHandle* e);
    void Ref(LRUHandle* e);
    void Unref(LRUHandle* e);
    bool FinishErase(LRUHandle* e);

   public:
    LRUCache();
    ~LRUCache();

    // 设置容量
    void SetCapacity(size_t capacity) { capacity_ = capacity; }

    // 缓存操作
    Cache::Handle* Insert(const std::string_view& key, uint32_t hash, void* value, size_t charge,
                          void (*deleter)(const std::string_view& key, void* value));

    Cache::Handle* Lookup(const std::string_view& key, uint32_t hash);
    void Release(Cache::Handle* handle);
    void Erase(const std::string_view& key, uint32_t hash);
    void Prune();
    size_t TotalCharge() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return usage_;
    }
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
    // 初始化为空的循环链表
    lru_.next = &lru_;
    lru_.prev = &lru_;

    in_use_.next = &in_use_;
    in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
    // 检查：所有条目都应该已经释放
    assert(in_use_.next == &in_use_);
    for (LRUHandle* e = lru_.next; e != &lru_;) {
        LRUHandle* next = e->next;
        assert(e->in_cache);
        e->in_cache = false;
        assert(e->refs == 1);  // LRU 链表的条目 refs 必须为 1
        Unref(e);              // 调用 deleter 并释放
        e = next;
    }
}

// ----------------------------------------------------------------------------
// 引用计数管理
// ----------------------------------------------------------------------------

/**
 * @brief 增加引用计数
 * @param e：缓存条目
 *
 * 当条目获得外部引用时：
 *  - 从 LRU 链表移动到 in_use 链表
 *  - refs++
 */
void LRUCache::Ref(LRUHandle* e) {
    if (e->refs == 1 && e->in_cache) {
        // 如果在 LRU 链表上
        LRU_Remove(e);
        LRU_Append(&in_use_, e);  // 移动到 in_use 链表
    }
    e->refs++;
}

/**
 * @brief 减少引用计数
 * @param e：缓存条目
 *
 * 当条目失去外部引用时：
 *  - refs--
 *  - 如果 refs==0：释放条目
 *  - 如果 refs==1 且 in_cache：移动到 LRU 链表
 */
void LRUCache::Unref(LRUHandle* e) {
    assert(e->refs > 0);
    e->refs--;
    if (e->refs == 0) {
        assert(!e->in_cache);
        (*e->deleter)(e->key(), e->value);
        free(e);
    } else if (e->in_cache && e->refs == 1) {
        LRU_Remove(e);
        LRU_Append(&lru_, e);
    }
}

// ----------------------------------------------------------------------------
// LRU 链表操作
// ----------------------------------------------------------------------------

/**
 * @brief 从链表删除条目
 * @param e：要删除的条目
 */
void LRUCache::LRU_Remove(LRUHandle* e) {
    e->next->prev = e->prev;
    e->prev->next = e->next;
}

/**
 * @brief 将条目追加到链表
 * @param list：链表头（哑元节点）
 * @param e：要追加的条目
 *
 * 将 e 插入到 list 之前（成为最新条目）
 */
void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
    e->next = list;
    e->prev = list->prev;
    e->prev->next = e;
    e->next->prev = e;
}

// ----------------------------------------------------------------------------
// 缓存核心操作
// ----------------------------------------------------------------------------

/**
 * @brief 查找键
 * @param key：键
 * @param hash：键的哈希值
 * @return Cache::Handle* 找到的句柄，未找到返回 nullptr
 *
 * 如果找到，增加引用计数并返回句柄
 * 调用者必须调用 Release() 释放句柄
 */
Cache::Handle* LRUCache::Lookup(const std::string_view& key, uint32_t hash) {
    std::lock_guard<std::mutex> lock(mtx_);
    LRUHandle* e = table_.Lookup(key, hash);
    if (e != nullptr) {
        Ref(e);
    }
    return reinterpret_cast<Cache::Handle*>(e);
}

/**
 * @brief 释放句柄
 * @param handle：要释放的句柄
 */
void LRUCache::Release(Cache::Handle* handle) {
    std::lock_guard<std::mutex> lock(mtx_);
    Unref(reinterpret_cast<LRUHandle*>(handle));
}

/**
 * @brief 插入键值对
 * @param key 键
 * @param hash 键的哈希值
 * @param value 值
 * @param charge 容量计费
 * @param deleter 删除回调
 * @return Cache::Handle* 插入条目的句柄
 *
 * 如果键已存在，替换旧条目
 * 如果超出容量，驱逐 LRU 条目
 */
Cache::Handle* LRUCache::Insert(const std::string_view& key, uint32_t hash, void* value, size_t charge,
                                void (*deleter)(const std::string_view& key, void* value)) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 分配内存：LRUHandle + 键数据（变长）
    LRUHandle* e = reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
    // 初始化条目
    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->key_length = key.size();
    e->hash = hash;
    e->in_cache = false;
    e->refs = 1;
    std::memcpy(e->key_data, key.data(), key.size());

    if (capacity_ > 0) {
        // 启用缓存
        e->refs++;  // 缓存自身的引用
        e->in_cache = true;
        LRU_Append(&in_use_, e);  // 加入 in_use 链表
        usage_ += charge;
        FinishErase(table_.Insert(e));  // 删除旧的同键条目（如果有）
    } else {
        // 禁用缓存（capacity_ == 0）
        e->next = nullptr;
    }

    // 如果超出容量，驱逐 LRU 条目
    while (usage_ > capacity_ && lru_.next != &lru_) {
        LRUHandle* old = lru_.next;
        assert(old->refs == 1);
        bool erased = FinishErase(table_.Remove(old->key(), old->hash));
        if (!erased) {
            assert(erased);
        }
    }
    return reinterpret_cast<Cache::Handle*>(e);
}

/**
 * @brief 完成删除操作
 * @param e 要删除的条目（已从哈希表移除）
 * @return bool e 是否非 nullptr
 *
 * 从 LRU 链表移除，减少引用，可能触发释放
 */
bool LRUCache::FinishErase(LRUHandle* e) {
    if (e != nullptr) {
        assert(e->in_cache);
        LRU_Remove(e);
        e->in_cache = false;
        usage_ -= e->charge;
        Unref(e);
    }
    return e != nullptr;
}

/**
 * @brief 删除键
 * @param key：键
 * @param hash：键的哈希值
 */
void LRUCache::Erase(const std::string_view& key, uint32_t hash) {
    std::lock_guard<std::mutex> lock(mtx_);
    FinishErase(table_.Remove(key, hash));
}

/**
 * @brief 清理所有未使用的条目
 * 强制驱逐 LRU 链表中的所有条目
 * 用于内存首先场景
 */
void LRUCache::Prune() {
    std::lock_guard<std::mutex> lock(mtx_);
    while (lru_.next != &lru_) {
        LRUHandle* e = lru_.next;
        assert(e->refs == 1);
        bool erased = FinishErase(table_.Remove(e->key(), e->hash));
        if (!erased) {
            assert(erased);
        }
    }
}

// ============================================================================
// ShardedLRUCache - 分片缓存（最终实现）
// ============================================================================

// 分片数量：2^4 = 16 个分片
static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

/**
 * @brief 分片 LRU 缓存
 *
 * 设计目的：
 *  - 减少锁竞争，提高并发性能
 *  - 16 个分片，每个分片独立锁
 *
 * 分片选择：
 *  - 使用哈希值的高 4 位选择分片
 *  - 均匀分布键到各分片
 */
class SharedLRUCache : public Cache {
   private:
    LRUCache shared_[kNumShards];  // 16 个分片
    std::mutex id_mtx_;
    uint64_t last_id_;

    // 计算键的哈希值
    static inline uint32_t HashSlice(const std::string_view& s) { return Hash(s.data(), s.size(), 0); }

    // 根据哈希值选择分片（取高 4 位）
    static uint32_t Shared(uint32_t hash) { return hash >> (32 - kNumShardBits); }

   public:
    explicit SharedLRUCache(size_t capacity) : last_id_(0) {
        // 容量均分到各分片
        const size_t per_shared = (capacity + (kNumShards - 1)) / kNumShards;
        for (int s = 0; s < kNumShards; s++) {
            shared_[s].SetCapacity(per_shared);
        }
    }

    ~SharedLRUCache() override {}

    /**
     * @brief 插入键值对
     *
     * 根据哈希值选择分片，调用对应分片的 Insert
     */
    Handle* Insert(const std::string_view& key, void* value, size_t charge,
                   void (*deleter)(const std::string_view& key, void* value)) override {
        const uint32_t hash = HashSlice(key);
        return shared_[Shared(hash)].Insert(key, hash, value, charge, deleter);
    }

    /**
     * @brief 查找键
     *
     * 根据哈希值选择分片，调用对应分片的 Lookup
     */
    Handle* Lookup(const std::string_view& key) override {
        const uint32_t hash = HashSlice(key);
        return shared_[Shared(hash)].Lookup(key, hash);
    }

    /**
     * @brief 释放句柄
     *
     * 从句柄中提取哈希值，找到对应分片
     */
    void Release(Handle* handle) override {
        LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
        shared_[Shared(h->hash)].Release(handle);
    }

    /**
     * 删除键
     */
    void Erase(const std::string_view& key) override {
        const uint32_t hash = HashSlice(key);
        shared_[Shared(hash)].Erase(key, hash);
    }

    /**
     * 获取句柄的值
     */
    void* Value(Handle* handle) override { return reinterpret_cast<LRUHandle*>(handle)->value; }

    /**
     * 生成新 ID
     *
     * 用于客户端分区键空间
     */
    uint64_t NewId() override {
        std::lock_guard<std::mutex> lock(id_mtx_);
        return ++last_id_;
    }

    /**
     * 清理所有分片
     */
    void Prune() override {
        for (int s = 0; s < kNumShards; s++) {
            shared_[s].Prune();
        }
    }

    /**
     * @brief 返回总占用容量
     */
    size_t TotalCharge() const override {
        size_t total = 0;
        for (int s = 0; s < kNumShards; s++) {
            total += shared_[s].TotalCharge();
        }
        return total;
    }
};

}  // namespace

// ============================================================================
// 公共 API：创建 LRU 缓存
// ============================================================================

/**
 * @brief 创建固定容量的 LRU 缓存
 * @param capacity：缓存容量
 * @return Cache*：新创建的缓存对象
 *
 * 实际创建的是 SharedLURCache （16 分片）
 * 调用者负责 delete 返回的缓存对象
 */
Cache* NewLRUCache(size_t capacity) { return new SharedLRUCache(capacity); }

}  // namespace delta