#pragma once

#include "dbformat.h"
#include "iterator.h"
#include "skiplist.h"

namespace delta {

class InternalKeyComparator;
class MemTableIterator;

/**
 * 内存表，用于存储最新写入但尚未持久化到磁盘的数据
 * 写入 → MemTable.Add() → 满后冻结为 Immutable MemTable → Compaction 刷盘
           ↓
       读取时先查 MemTable.Get()
 */
class MemTable {
   private:
    friend class MemTableIterator;
    friend class MemTableBackwardIterator;

    struct KeyComparator {
        const InternalKeyComparator comparator;

        explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}

        int operator()(const char* a, const char* b) const;
    };

    typedef SkipList<const char*, KeyComparator> Table;

    ~MemTable();

    KeyComparator comparator_;  // internal key 比较器
    int refs_;                  // 引用计数
    Arena arena_;               // 内存池分配器，跳表节点从 arena 分配
    Table table_;               // 底层跳表，存储实际的 key/value 数据

   public:
    explicit MemTable(const InternalKeyComparator& comparator);

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // 增加引用计数（线程不安全，由外部同步）
    void Ref() { refs_++; }

    // 减少引用计数，计数为0时自动删除自身
    void Unref() {
        refs_--;
        assert(refs_ >= 0);
        if (refs_ <= 0) {
            delete this;
        }
    }

    // 返回 MemTable 当前占用内存的估计值
    size_t ApproximateMemoryUsage();

    // 创建迭代器遍历 MemTable 内容
    Iterator* NewIterator();

    // 向 MemTable 添加一条记录（带序列号和类型）
    void Add(SequenceNumber seq, ValueType type, const std::string_view& key, const std::string_view& value);

    // 查找 key，存在则返回 value；是删除标记则返回 NotFound 错误
    bool Get(const LookupKey& key, std::string* value, Status* s);
};

}  // namespace delta