#pragma once

#include <cstdint>

#include <deltadb/db/db.h>
#include <deltadb/utils/dbformat.h>

namespace delta {

class DBImpl;

/**
 * @brief 负责创建数据库迭代器，将底层的内部键转换为用户键，并实现 MVCC （多版本并发控制）的可见性判断。
 * 作用就是将内部键"解码"为用户键，并过滤掉不可见的版本。
 * @param db：数据库实例指针，用于访问 MemTable 和 SSTable
 * @param user_key_comparator：用户键比较器，用于比较用户可见的键
 * @param internal_iter：底层迭代器，遍历所有内部键（包含多个版本）
 * @param sequence：序列号，用于 MVCC 的可见性判断（只返回该序列号之前写入的数据）
 * @param seed：随机种子，用于某些随机化行为（如防止某些攻击）
 * @return 返回一个迭代器，该迭代器遍历时只返回对该序列号可见的、用户格式的键值对
 *
 * 用户键：用户存储的原始键，如 "user:123"
    内部键 = 用户键 + 序列号 + 类型
    ┌─────────────────────────────────────┐
    │  "user:123" │ seq=100 │ kTypeValue  │
    └─────────────────────────────────────┘
 */
Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed);

}  // namespace delta