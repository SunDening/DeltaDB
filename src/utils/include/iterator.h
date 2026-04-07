#pragma once

#include <assert.h>

#include "status.h"

namespace delta {

/**
 * 定义了核心迭代器的抽象接口，是整个项目中数据访问的基石。
 * LevelDB 使用迭代器组合来实现复杂查询。以下是项目中主要的迭代器实现：
┌─────────────────────────────────────────────────────────┐
│ DBIter (db/db_iter.cc)                                  │
│ - 封装整个 DB 的视图，处理快照和删除标记                 │
└─────────────────────────────────────────────────────────┘
                          ↓ 组合
┌─────────────────────────────────────────────────────────┐
│ MergingIterator (table/merger.cc)                       │
│ - 归并多个子迭代器（用于多文件归并）                     │
└─────────────────────────────────────────────────────────┘
                          ↓ 组合
┌─────────────────────┐           ┌───────────────────────┐
│ MemTableIterator    │           │ TwoLevelIterator      │
│ (db/memtable.cc)    │           │ (table/two_level_...) │
│ - 遍历内存表         │           │ - 遍历 SST 文件        │
└─────────────────────┘           └───────────────────────┘
                                            ↓
                                  ┌───────────────────────┐
                                  │ Block::Iter           │
                                  │ (table/block.cc)      │
                                  │ - 遍历数据块          │
                                  └───────────────────────┘
 *
 * Iterator 是一个抽象基类，定义了访问键值对的标准方法：
 *  方法	功能
    Valid()	检查迭代器是否在有效位置
    SeekToFirst() / SeekToLast()	定位到首/尾
    Seek(target)	定位到 ≥ target 的第一个键
    Next() / Prev()	向前/向后移动
    key() / value()	获取当前键/值
    status()	获取错误状态
 *
 * 上层迭代器可以组合多个下层迭代器，形成查询链: 从内存表 → SST 文件 → 数据库视图
 *
 */

class Iterator {
   public:
    Iterator() = default;

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    virtual ~Iterator() = default;

    // 检查迭代器是否处于有效位置（是否指向一个有效的 key/value 对）
    virtual bool Valid() const = 0;

    // 将迭代器定位到第一个 key（如果数据源非空，则迭代器变为 Valid）
    virtual void SeekToFirst() = 0;

    // 将迭代器定位到最后一个 key
    virtual void SeekToLast() = 0;

    // 定位到第一个大于或等于 target 的 key
    virtual void Seek(const std::string_view& target) = 0;

    // 移动到下一个条目（要求调用前 Valid() 为true）
    virtual void Next() = 0;

    // 移动到上一个条目（要求调用前 Valid() 为true）
    virtual void Prev() = 0;

    // 返回当前 entry 的 key
    virtual std::string_view key() const = 0;

    // 返回当前 entry 的 value
    virtual std::string_view value() const = 0;

    // 如果发生错误则返回错误状态，否则返回 OK 状态
    virtual Status status() const = 0;

    using CleanupFunction = void (*)(void* arg1, void* arg2);

    // 注册清理函数，在迭代器销毁时会被调用（用于资源管理）
    void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

   private:
    // CleanupNode 结构体是 Iterator 类内部用于管理资源清理的私有结构
    struct CleanupNode {
        CleanupFunction function;  // 清理函数指针
        void* arg1;
        void* arg2;
        CleanupNode* next;  // 指向下一个 CleanupNode，形成单向链表

        // 判断节点是否未被使用
        bool IsEmpty() const { return function == nullptr; }

        // 执行清理函数
        void Run() {
            assert(function != nullptr);
            (*function)(arg1, arg2);
        }
    };

    CleanupNode cleanup_head_;
};

// 创建一个空迭代器（不产生任何数据）
Iterator* NewEmptyIterator();

// 创建一个带有指定错误状态的迭代器
Iterator* NewErrorIterator(const Status& status);

}  // namespace delta