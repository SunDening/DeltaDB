#pragma once

#include <cstddef>
#include <cstdint>

#include "iterator.h"

namespace delta {

/**
 * Block 是 SST 文件中数据的基本组织单元，用于：
 *  - 存储排序后的键值对序列
 *  - 支持高效的前向扫描
 *  - 支持二分查找定位
 *
 * Block 数据结构：
    ┌─────────────────────────────────────────────────────────┐
    │  共享键前缀长度 (varint32)                               │
    ├─────────────────────────────────────────────────────────┤
    │  非共享后缀长度 (varint32)                               │
    ├─────────────────────────────────────────────────────────┤
    │  值长度 (varint32)                                       │
    ├─────────────────────────────────────────────────────────┤
    │  非共享键后缀 (变长)                                      │
    ├─────────────────────────────────────────────────────────┤
    │  值数据 (变长)                                           │
    ├─────────────────────────────────────────────────────────┤
    │  下一条记录...                                          │
    ├─────────────────────────────────────────────────────────┤
    │  重启点数组 (uint32[k])                                 │
    │  - 每个重启点是一条记录的起始偏移                         │
    │  - 每隔 block_restart_interval 条记录一个重启点            │
    ├─────────────────────────────────────────────────────────┤
    │  重启点数量 (uint32)                                     │
    └─────────────────────────────────────────────────────────┘
 *
 * 键前缀压缩：
    - 记录 1: key="apple", value="1"
    - 记录 2: key="apply", value="2"  ← 只存 "y"
    - 记录 3: key="banana", value="3"
    优势：相邻键通常有共同前缀，压缩后节省空间。
 *
 * 重启点（Restart Points）
    数据区：
    记录 0 ← 重启点 0 (偏移 0)
    记录 1
    记录 2
    ...
    记录 15
    记录 16 ← 重启点 1 (偏移 X)
    记录 17
    ...

    重启点数组：
    [0, X, Y, Z, ...]
        ↑
    重启点数量：k
    作用：
        - 每隔 block_restart_interval（默认 16）条记录设置一个重启点
        - 重启点存储完整键（无前缀压缩）
        - 支持二分查找快速定位
 *
 */

// struct BlockContents {
//   Slice data;           // 数据指针
//   bool cachable;        // 是否可缓存
//   bool heap_allocated;  // 是否堆分配
// };
struct BlockContents;  // 包含数据指针和所有权信息

class Comparator;

class Block {
   private:
    class Iter;  // 内部迭代器，实现于 block.cc

    const char* data_;         // 数据起始指针
    size_t size_;              // 数据大小
    uint32_t restart_offset_;  // 重启点数组的偏移
    bool owned_;               // 是否拥有 data_ 的所有权

    uint32_t RestartsNum() const;  // 返回重启点数量(存储在最后 4 字节)

   public:
    explicit Block(const BlockContents& contents);

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    ~Block();

    // 返回 Block 的字节大小（用于缓存计费）
    size_t size() const { return size_; }

    // 为该 Block 创建迭代器，支持顺序遍历和查找
    Iterator* NewIterator(const Comparator* comparator);
};

}  // namespace delta