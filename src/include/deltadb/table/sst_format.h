#pragma once

#include <cstdint>

#include <deltadb/table/sst_builder.h>
#include <deltadb/utils/status.h>

namespace delta {

/**
 * 定义了 SST 文件的物理存储格式，包括：
 *  - BlockHandle - 块的指针（偏移 + 大小）
 *  - Footer - 文件尾部元数据
 *  - BlockContents - 块的内存表示
 *  - 读取函数的接口
 */

class Block;
class RandomAccessFile;

struct ReadOptions;

static const uint64_t kSSTMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
static const size_t kBlockTrailerSize = 5;

/**
 * 块句柄，指向文件中一个块的指针
 */
class BlockHandle {
   private:
    uint64_t offset_;  // 块在文件中的偏移
    uint64_t size_;    // 块的大小

   public:
    /**
     * BlockHandle 编码（变长）：
    ┌──────────────────┬──────────────────┐
    │ offset (varint)  │ size (varint)    │
    │ 最多 10 字节       │ 最多 10 字节      │
    └──────────────────┴──────────────────┘
       最大 20 字节
     */
    enum { kMaxEncodedLength = 10 + 10 };

    BlockHandle();

    uint64_t offset() const { return offset_; }
    void set_offset(uint64_t offset) { offset_ = offset; }

    uint64_t size() const { return size_; }
    void set_size(uint64_t size) { size_ = size; }

    // 编码到字符串
    void EncodeTo(std::string* dst) const;
    // 解码
    Status DecodeFrom(std::string_view* input);
};

/**
 * 存储在 SST 文件末尾的固定长度元数据，包含：
 *  - MetaIndexBlock 的位置
 *  - IndexBlock 的位置
 *  - Magic Number（文件标识）
 *
 * Footer 编码（固定 48 字节）：
    ┌─────────────────────────────────────────────────────────┐
    │ MetaIndex Handle (varint, 最多 20 字节)                   │
    ├─────────────────────────────────────────────────────────┤
    │ Index Handle (varint, 最多 20 字节)                       │
    ├─────────────────────────────────────────────────────────┤
    │ 填充字节 (使 Magic Number 位于文件末尾)                    │
    ├─────────────────────────────────────────────────────────┤
    │ Magic Number (8 字节) 0xdb4775248b80fb57                │
    └─────────────────────────────────────────────────────────┘
 *
 */
class Footer {
   private:
    BlockHandle metaindex_handle_;  // 元数据索引块
    BlockHandle index_handle_;      // 数据索引块

   public:
    // Footer 编码固定 48 字节
    enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

    Footer() = default;

    const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
    void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

    const BlockHandle& index_handle() const { return index_handle_; }
    void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(std::string_view* input);
};

/**
 * 块内容，Block 在内存中的表示，包含数据和控制信息。
 *
 * 读取 Block 的三种方式：
    ┌─────────────────────────────────────────────────────────┐
    │ 1. 直接映射（mmap）                                      │
    │    - cachable = true                                    │
    │    - heap_allocated = false                             │
    ├─────────────────────────────────────────────────────────┤
    │ 2. 堆分配（new char[]）                                  │
    │    - cachable = true                                    │
    │    - heap_allocated = true                              │
    ├─────────────────────────────────────────────────────────┤
    │ 3. 临时缓冲区（栈/成员变量）                              │
    │    - cachable = false                                   │
    │    - heap_allocated = false                             │
    └─────────────────────────────────────────────────────────┘
 */
struct BlockContents {
    std::string_view data;  // 实际数据
    bool cacheable;         // 是否可缓存
    bool heap_allocated;    // 是否堆分配（需要 delete[]）
};

/**
 * @brief 读取块
 * @param file 随机访问文件
 * @param options 读选项（是否验证校验和）
 * @param handle 块句柄（偏移 + 大小）
 * @param result 输出：块内容
 * @return Status 状态
 *
 * 执行流程：
 *  1. 验证块大小（不超过 2GB）
 *  2. 读取数据 + 尾部（5 字节）
 *      - 数据区：handle.size() 字节
 *      - 尾部：1 字节类型 + 4 字节 CRC
 *  3. 验证 CRC 校验和
 *  4. 解压缩（如果需要）
 *  5. 填充 BlockContents 并返回
 */
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle, BlockContents* result);

inline BlockHandle::BlockHandle() : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace delta