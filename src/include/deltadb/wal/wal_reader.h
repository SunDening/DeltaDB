#pragma once

#include <deltadb/wal/sequential_file.h>
#include <deltadb/wal/wal_format.h>

/**
 * WAL 读取器
 * 是预写日志的核心读取组件，负责从日志文件中安全地恢复数据
 * 日志格式设计中最精妙之处在于块边界对齐，当检测到损坏时，可以直接跳到下一个块边界重新扫描，
 * 这避免了复杂的数据同步启发式算法，让错误恢复变得简单可靠。
 */

namespace delta {

class Reader {
   public:
    /**
     * 错误报告接口. 通过回调报告损坏情况
     * wal_reader 采用宽松的错误处理，优先保证数据可用性：
     *  CRC不匹配：丢弃当前块缓冲区，报告损坏，继续读取
     *  记录损坏：返回 kBadRecord，调用者决定是否继续
     *  不完整记录：返回 kEof，但不报告损坏（可能是写崩溃导致）
     *  片段丢失：清空 scratch，重新寻找完整记录
     */
    class Reporter {
       public:
        virtual ~Reporter();

        // 检测到 corruption
        // bytes 是由于损坏而丢失的字节数
        virtual void Corruption(size_t bytes, const Status& status) = 0;
        ;
    };

   private:
    // 扩展的记录类型
    enum { kEof = kMaxRecordType + 1, kBadRecord = kMaxRecordType + 2 };

    SequentialFile* const file_;  // 顺序文件接口
    Reporter* const reporter_;
    bool const checksum_;        // 是否校验CRC
    char* const backing_store_;  // 32KB的块缓冲区
    std::string_view buffer_;    // 当前缓冲区内容
    bool eof_;                   // 是否已到文件末尾

    uint64_t last_record_offset_;    // ReadRecord返回的最后一条记录的偏移量
    uint64_t end_of_buffer_offset_;  // 缓冲区结束后第一个位置的偏移量
    uint64_t const initial_offset_;  // 开始查找要返回的第一条记录的偏移量

    bool resyncing_;  // 是否正在从initial_offset_重新同步

    bool SkipToInitialBlock();  // 跳过所有完全在 initial_offset_ 之前的块

    unsigned int ReadPhysicalRecord(std::string_view* result);  // 读取物理记录，返回记录类型，或上述特殊值之一

    void ReportCorruption(uint64_t bytes, const char* reason);
    void ReportDrop(uint64_t bytes, const Status& reason);

   public:
    Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset);

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    ~Reader();

    // 读取逻辑记录，负责组装块的分片记录
    bool ReadRecord(std::string_view* record, std::string* scratch);

    // 返回由ReadRecord返回的最后一条记录的物理偏移量。
    uint64_t LastRecordOffset();
};

}  // namespace delta
