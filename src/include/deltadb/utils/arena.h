#pragma once

#include <atomic>
#include <cassert>
#include <vector>

namespace delta {

/**
 * Arena 是一个内存分配器，专门用于高效地分配和管理大量小对象的内存。
 * 它通过预分配大块内存并在其中分配小对象来减少内存碎片和分配开销。
 * 适用于需要频繁分配和释放小对象的场景，如数据库中的记录、索引等。
 * 通过使用 Arena，WAL 可以快速地分配和管理日志记录的内存，减少内存碎片，并且在需要回收内存时，可以通过释放整个 Arena
 * 来高效地回收所有内存，从而提高性能和减少内存碎片。
 */
class Arena {
   private:
    // 当前块的分配状态
    char* alloc_ptr_;               // 当前块的分配指针
    size_t alloc_bytes_remaining_;  // 当前块剩余的可用字节数
    std::vector<char*> blocks_;     // 已分配的块列表

    // 总内存使用量（原子操作，支持并发读取）
    std::atomic<size_t> memory_usage_;

    // 回退策略
    char* AllocateFallback(size_t bytes);
    // 分配新的块
    char* AllocateNewBlock(size_t block_bytes);

   public:
    Arena();

    Arena(const Arena&) = delete;             // 禁止复制构造
    Arena& operator=(const Arena&) = delete;  // 禁止复制赋值
    ~Arena();                                 // 析构函数，释放所有分配的内存

    // 分配指定字节数的内存，并返回指向该内存的指针
    char* Allocate(size_t bytes);
    // 对齐分配，确保返回的指针满足指定的对齐要求
    char* AllocateAligned(size_t bytes);

    // 返回当前 Arena 使用的内存总量
    size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }
};

inline char* Arena::Allocate(size_t bytes) {
    assert(bytes > 0);

    // 如果当前块剩余空间足够，直接分配
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;        // 返回当前指针
        alloc_ptr_ += bytes;              // 指针后移
        alloc_bytes_remaining_ -= bytes;  // 减少剩余字节数
        return result;
    }

    // 空间不足，调用回退函数分配新块
    return AllocateFallback(bytes);
}
}  // namespace delta