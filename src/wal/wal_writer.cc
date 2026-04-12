#include <crc32c/crc32c.h>

#include <deltadb/utils/coding.h>
#include <deltadb/utils/log.h>
#include <deltadb/wal/wal_writer.h>

namespace delta {

static void InitTypeCrc(uint32_t* type_crc) {
    for (int i = 0; i <= kMaxRecordType; i++) {
        char t = static_cast<char>(i);
        type_crc[i] = crc32c::Crc32c(&t, 1);
    }
}

/**
 * 从空文件开始写入
 */
Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) { InitTypeCrc(type_crc_); }

/**
 * 追加到现有文件，根据文件长度计算块内偏移
 */
Writer::Writer(WritableFile* dest, uint64_t dest_length) : dest_(dest), block_offset_(dest_length % kBlockSize) {
    DebugLog << "Writer: 将预计算 crc32c ";
    InitTypeCrc(type_crc_);
    DebugLog << "Writer: 预计算 crc32c 完成";
}

Writer::~Writer() = default;

/**
 * 将用户记录写入 WAL，自动处理边界和记录分片
 */
Status Writer::AddRecord(const std::string_view slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();  // 记录未写的数据大小

    Status s;
    bool begin = true;  // 标记是否为记录的开始

    do {
        // 处理块边界. 块对齐减少磁盘碎片
        const int leftover = kBlockSize - block_offset_;  // 块内剩余容量
        if (leftover < kHeaderSize) {                     // 不足写 header
            // 用0填充尾部并切换到新块
            if (leftover > 0) {
                dest_->Append(std::string_view("\x00\x00\x00\x00\x00\x00", leftover));
            }
            block_offset_ = 0;  // 跳转到新块
        }

        // 计算可写入空间
        const size_t avail = kBlockSize - block_offset_ - kHeaderSize;  // 剩余的数据可用空间
        const size_t fragment_length = (left < avail) ? left : avail;   // 本块中该数据碎片的大小

        // 确定记录类型
        RecordType type;
        const bool end = (left == fragment_length);
        if (begin && end) {
            type = kFullType;  // 完整记录
        } else if (begin) {
            type = kFirstType;  // 第一个片段
        } else if (end) {
            type = kLastType;  // 最后一个片段
        } else {
            type = kMiddleType;  // 中间片段
        }

        // 写入物理记录
        s = EmitPhysicalRecord(type, ptr, fragment_length);

        // 更新指针和状态
        ptr += fragment_length;   // 右移未写数据起始指针
        left -= fragment_length;  // 未写数据大小更新
        begin = false;
    } while (s.ok() && left > 0);

    return s;
}

/**
 * 写入单条物理记录（7字节头部 + 数据体）
 * 头部：
        checksum (4字节)  : uint32, CRC32C 校验码，小端序
        length   (2字节)  : uint16, 数据长度，小端序
        type     (1字节)  : uint8, 记录类型
    数据：
        data     (N字节)   : 用户数据
 */
Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
    // 断言检查
    assert(length <= 0xffff);                                    // 长度必须适合2字节（相当于数据部分大小限制）
    assert(block_offset_ + kHeaderSize + length <= kBlockSize);  // 防止缓冲区溢出

    // 构造头部
    char buf[kHeaderSize];
    buf[4] = static_cast<char>(length & 0xff);  // 长度低字节
    buf[5] = static_cast<char>(length >> 8);    // 长度高字节
    buf[6] = static_cast<char>(t);              // 记录类型

    // 计算 CRC32C 校验（与数据部分合并计算）
    uint32_t crc = crc32c::Extend(type_crc_[t], reinterpret_cast<const uint8_t*>(ptr),
                                  length);  // 基于预计算值扩展数据 CRC
    EncodeFixed32(buf, crc);                // 写入头部前4字节

    // 写入头部和数据到缓冲区
    Status s = dest_->Append(std::string_view(buf, kHeaderSize));
    if (s.ok()) {
        s = dest_->Append(std::string_view(ptr, length));
        if (s.ok()) {
            s = dest_->Flush();  // 刷新到操作系统缓存
        }
    }

    // 更新块偏移
    block_offset_ += kHeaderSize + length;
    return s;
}

}  // namespace delta