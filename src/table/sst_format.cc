#include <crc32c/crc32c.h>

#include "block.h"
#include "coding.h"
#include "random_access_file.h"
#include "sst_format.h"

namespace delta {

// ============================================================================
// BlockHandle 编码/解码实现
// ============================================================================

/**
 * @brief 将 BlockHandle 编码到字符串
 * @param dst 目标字符串
 *
 * 编码格式（变长，最多 20 字节）：
 * ┌──────────────────┬──────────────────┐
 * │ offset (varint)  │ size (varint)    │
 * │ 最多 10 字节       │ 最多 10 字节      │
 * └──────────────────┴──────────────────┘
 *
 * 前置条件：offset_ 和 size_ 必须已设置（不为 ~0）
 */
void BlockHandle::EncodeTo(std::string* dst) const {
    assert(offset_ != ~static_cast<uint64_t>(0));
    assert(size_ != ~static_cast<uint64_t>(0));
    PutVarint64(dst, offset_);
    PutVarint64(dst, size_);
}

/**
 * @brief 从 Slice 解码 BlockHandle
 * @param input 输入数据（会修改，移除已读取的字节）
 * @return Status 状态
 *
 * 解码失败情况：
 * - 数据不足（无法解析两个 varint）
 * - 数据损坏
 */
Status BlockHandle::DecodeFrom(std::string_view* input) {
    if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
        return Status::OK();
    } else {
        return Status::Corruption("bad block handle");
    }
}

// ============================================================================
// Footer 编码/解码实现
// ============================================================================

/**
 * @brief 将 Footer 编码到字符串
 * @param dst 目标字符串
 *
 * 编码格式（固定 48 字节）：
 * ┌─────────────────────────────────────────────────────────┐
 * │ MetaIndex Handle (varint, 最多 20 字节)                   │
 * ├─────────────────────────────────────────────────────────┤
 * │ Index Handle (varint, 最多 20 字节)                       │
 * ├─────────────────────────────────────────────────────────┤
 * │ 填充字节 (使总长度达到 40 字节)                            │
 * ├─────────────────────────────────────────────────────────┤
 * │ Magic Number (8 字节，小端序) 0xdb4775248b80fb57         │
 * └─────────────────────────────────────────────────────────┘
 *
 * 设计要点：
 * - 固定长度便于从文件末尾读取
 * - Magic Number 用于验证文件合法性
 */
void Footer::EncodeTo(std::string* dst) const {
    const size_t original_size = dst->size();

    // 编码两个 BlockHandle
    metaindex_handle_.EncodeTo(dst);
    index_handle_.EncodeTo(dst);

    // 填充到固定长度（40 字节 = 2 * 20）
    // 如果实际编码小于 40 字节，用空格填充
    dst->resize(2 * BlockHandle::kMaxEncodedLength);

    // 添加 Magic Number（小端序，分两次写入 32 位）
    PutFixed32(dst, static_cast<uint32_t>(kSSTMagicNumber & 0xffffffffu));
    PutFixed32(dst, static_cast<uint32_t>(kSSTMagicNumber >> 32));

    // 验证总长度：原始长度 + 48 字节
    assert(dst->size() == original_size + kEncodedLength);
    (void)original_size;  // 禁用未使用变量警告
}

/**
 * @brief 从 Slice 解码 Footer
 * @param input 输入数据（会修改）
 * @return Status 状态
 *
 * 验证步骤：
 * 1. 检查长度是否 >= 48 字节
 * 2. 验证 Magic Number
 * 3. 解码两个 BlockHandle
 * 4. 跳过填充数据
 */
Status Footer::DecodeFrom(std::string_view* input) {
    // 验证长度
    if (input->size() < kEncodedLength) {
        return Status::Corruption("not an sst (footer too short)");
    }

    // 定位 Magic Number（最后 8 字节）
    const char* magic_ptr = input->data() + kEncodedLength - 8;

    // 读取 Magic Number （小端序）
    const uint32_t magic_low = DecodeFixed32(magic_ptr);
    const uint32_t magic_high = DecodeFixed32(magic_ptr + 4);
    const uint64_t magic = ((static_cast<uint64_t>(magic_high) << 32) | (static_cast<uint64_t>(magic_low)));

    // 验证 Magic Number
    if (magic != kSSTMagicNumber) {
        return Status::Corruption("not an sst (bad magic number)");
    }

    // 解码 MetaIndex Handle
    Status result = metaindex_handle_.DecodeFrom(input);
    if (result.ok()) {
        // 解码 Index Handle
        result = index_handle_.DecodeFrom(input);
    }
    if (result.ok()) {
        // 跳过填充数据（如果有）
        // input 将指向 Magic Number 之后的数据
        const char* end = magic_ptr + 8;
        *input = std::string_view(end, input->data() + input->size() - end);
    }
    return result;
}

// ============================================================================
// ReadBlock - 读取数据块
// ============================================================================

/**
 * @brief 从文件读取一个数据块
 *
 * @param file 随机访问文件
 * @param options 读选项（是否验证校验和）
 * @param handle 块句柄（偏移 + 大小）
 * @param result 输出：块内容
 * @return Status 状态
 *
 * 读取的数据格式：
 * ┌─────────────────────────────────────────┐
 * │ 压缩数据 (handle.size() 字节)             │
 * ├─────────────────────────────────────────┤
 * │ 压缩类型 (1 字节)                         │
 * │ - kNoCompression = 0x00                 │
 * │ - kSnappyCompression = 0x01             │
 * │ - kZstdCompression = 0x02               │
 * ├─────────────────────────────────────────┤
 * │ CRC32C 校验和 (4 字节，小端序)             │
 * └─────────────────────────────────────────┘
 *
 * 返回的 BlockContents 三种情况：
 * 1. 直接映射：cachable=false, heap_allocated=false
 * 2. 堆分配：cachable=true, heap_allocated=true
 * 3. 解压后：cachable=true, heap_allocated=true
 */
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle, BlockContents* result) {
    // 初始化输出
    result->data = std::string_view();
    result->cacheable = false;
    result->heap_allocated = false;

    // 读取数据 + 尾部
    // 读取大小 = 数据区 + 尾部（5 字节：1 字节类型 + 4 字节 CRC）
    size_t n = static_cast<size_t>(handle.size());
    char* buf = new char[n + kBlockTrailerSize];

    std::string_view contents;
    Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
    if (!s.ok()) {
        delete[] buf;
        return s;
    }

    // 验证读取的字节数
    if (contents.size() != n + kBlockTrailerSize) {
        delete[] buf;
        return Status::Corruption("truncated block read");
    }

    // 验证 CRC 校验和
    const char* data = contents.data();
    if (options.verify_checksums) {
        const uint32_t crc = DecodeFixed32(data + n + 1);
        const uint32_t actual = crc32c::Crc32c(data, n + 1);
        if (actual != crc) {
            delete[] buf;
            s = Status::Corruption("block checksum mismatch");
            return s;
        }
    }

    // 根据压缩类型处理
    switch (data[n]) {
        case kNoCompression: {
            if (data != buf) {
                // 文件实现返回了其他数据指针（如 mmap 映射）
                // 假设文件打开期间数据一直有效
                delete[] buf;
                result->data = std::string_view(data, n);
                result->heap_allocated = false;
                result->cacheable = false;
            } else {
                // 使用我们分配的缓冲区
                result->data = std::string_view(buf, n);
                result->heap_allocated = true;
                result->cacheable = true;
            }
            break;
        }

        case kSnappyCompression: {
            size_t ulength = 0;
            // 获取解压后的长度
            if (!Snappy_GetUncompressedLength(data, n, &ulength)) {
                delete[] buf;
                return Status::Corruption("corrupted snappy compressed block length");
            }

            // 分配解压缓冲区
            char* ubuf = new char[ulength];
            if (!Snappy_Uncompress(data, n, ubuf)) {
                delete[] buf;
                delete[] ubuf;
                return Status::Corruption("corrupted snappy compressed block contents");
            }
            // 清理临时缓冲区，使用解压后的数据
            delete[] buf;
            result->data = std::string_view(ubuf, ulength);
            result->heap_allocated = true;
            result->cacheable = true;
            break;
        }
        case kZstdCompression: {
            size_t ulength = 0;
            if (!Zstd_GetUncompressedLength(data, n, &ulength)) {
                delete[] buf;
                return Status::Corruption("corrupted zstd compressed block length");
            }
            char* ubuf = new char[ulength];
            if (!Zstd_Uncompress(data, n, ubuf)) {
                delete[] buf;
                delete[] ubuf;
                return Status::Corruption("corrupted zstd compressed block contents");
            }
            delete[] buf;
            result->data = std::string_view(ubuf, ulength);
            result->heap_allocated = true;
            result->cacheable = true;
            break;
        }
        default:
            delete[] buf;
            return Status::Corruption("bad block type");
    }
    return Status::OK();
}

}  // namespace delta