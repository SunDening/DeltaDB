#include <deltadb/utils/arena.h>
#include <deltadb/utils/log.h>

namespace delta {

static const int kBlockSize = 4096;  // 每个块的大小，通常为4KB

/**
 * 初始状态：没有分配任何块
 * alloc_ptr_ 和 alloc_bytes_remaining_ 都指向空状态，表示当前没有可用的内存块
 */
Arena::Arena() : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) { InfoLog << "Arena init complate."; }

/**
 * 不释放单个对象：只释放整个块
 * 无需析构对象：假设对象在 Arena 外部管理
 * O(n)
 */
Arena::~Arena() {
    for (size_t i = 0; i < blocks_.size(); i++) {
        delete[] blocks_[i];  // 释放每个分配的块
    }
}

/**
 * 分配策略：
 *  1. 请求大小 >1KB：直接分配一个独立块，避免浪费
 *  2. 请求大小 <=1KB：尝试在当前块分配，如果不足则分配一个新块
 * 注意：只有在当前块不足以满足请求时才会执行该策略
 */
char* Arena::AllocateFallback(size_t bytes) {
    // 如果请求大于块大小的 1/4，直接分配一个独立块
    if (bytes > kBlockSize / 4) {
        char* result = AllocateNewBlock(bytes);
        return result;
    }

    // 否则，分配一个新的块并在其中分配（当前块剩余的少量字节直接浪费掉）
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;
    char* result = alloc_ptr_;        // 返回新块的起始地址
    alloc_ptr_ += bytes;              // 更新分配指针
    alloc_bytes_remaining_ -= bytes;  // 更新剩余字节数
    return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
    char* result = new char[block_bytes];  // 分配一个新的块
    blocks_.push_back(result);             // 将新块添加到块列表中

    // 更新内存使用量：块大小 + 块指针的大小（blocks_ 中存储了指向块的指针）
    memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);  // 更新内存使用量
    return result;
}

/**
 * 对齐分配：确保返回的指针满足特定的对齐要求（通常为8字节）
 */
char* Arena::AllocateAligned(size_t bytes) {
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;  // 对齐要求：指针大小（8或16字节）

    // 计算需要的填充字节数，以满足对齐要求
    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);  // 当前指针的对齐偏移
    size_t slop = (current_mod == 0) ? 0 : align - current_mod;                  // 需要的填充字节数
    size_t needed = bytes + slop;                                                // 总共需要的字节数

    char* result;
    if (needed <= alloc_bytes_remaining_) {
        // 当前块有足够空间，直接分配
        result = alloc_ptr_ + slop;        // 返回对齐后的地址
        alloc_ptr_ += needed;              // 更新分配指针
        alloc_bytes_remaining_ -= needed;  // 更新剩余字节数
    } else {
        // 当前块空间不足，分配一个新的块
        result = AllocateFallback(bytes);  // AllocateFallback 会处理对齐问题
    }

    // 验证对齐
    assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);  // 确保返回的指针满足对齐要求
    return result;
}

}  // namespace delta