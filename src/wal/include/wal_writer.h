#pragma once

#include "wal_format.h"
#include "writable_file.h"

namespace delta {
class Writer {
   public:
    Writer(WritableFile* dest);
    Writer(WritableFile* dest, uint64_t dest_length);
    ~Writer();

    Status AddRecord(const std::string_view slice);  // 公开接口. 将用户记录写入 WAL，自动处理边界和记录分片

   private:
    Status EmitPhysicalRecord(RecordType type, const char* ptr,
                              size_t length);  // 写入单条物理记录（7字节头部 + 数据体）

    WritableFile* dest_;                     // 底层文件接口
    int block_offset_;                       // 当前块内的偏移量
    uint32_t type_crc_[kMaxRecordType + 1];  // 预计算的 CRC 值
};
}  // namespace delta