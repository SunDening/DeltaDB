#include <crc32c/crc32c.h>

#include <deltadb/utils/coding.h>
#include <deltadb/utils/log.h>
#include <deltadb/wal/wal_reader.h>

namespace delta {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }

uint64_t Reader::LastRecordOffset() { return last_record_offset_; }

void Reader::ReportCorruption(uint64_t bytes, const char* reason) { ReportDrop(bytes, Status::Corruption(reason)); }

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
    if (reporter_ != nullptr && end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
        reporter_->Corruption(static_cast<size_t>(bytes), reason);
    }
}

/**
 * 定位起始块
 * 用于从指定偏移处开始读取，支持增量恢复
 * 智能处理：
 *  如果偏移落在尾部（最后6字节），自动跳到下一个块
 *  因为记录不可能在尾部开始（至少需要7字节头部）
 */
bool Reader::SkipToInitialBlock() {
    const size_t offset_in_block = initial_offset_ % kBlockSize;        // 块内偏移
    uint64_t block_start_location = initial_offset_ - offset_in_block;  // 最后一个块的起始位置

    // 如果块内偏移在块的最后6个字节内，直接跳过该块，从下个块开始
    if (offset_in_block > kBlockSize - 6) {
        block_start_location += kBlockSize;
    }

    end_of_buffer_offset_ = block_start_location;

    // 跳到可以包含初始记录的第一个块的开始
    if (block_start_location > 0) {
        Status skip_status = file_->Skip(block_start_location);
        if (!skip_status.ok()) {
            ReportDrop(block_start_location, skip_status);
            return false;
        }
    }

    return true;
}

/**
 * 读取物理记录
 * 负责从缓冲区中解析单个记录头和数据
 * 步骤：
 *  1. 缓冲区管理：当缓冲区不足一个头部（7字节）时，从文件中读取一个新块（32KB）
 *  2. 头部解析：提取 checksum、length、type
 *  3. 长度校验：检查数据是否完整（header + length <= buffer_size）
 *  4. CRC 校验：如果启用，验证数据完整性
 *  5. 偏移检查：跳过 initial_offset 之前的记录
 */
unsigned int Reader::ReadPhysicalRecord(std::string_view* result) {
    while (true) {
        if (buffer_.size() < kHeaderSize) {
            if (!eof_) {
                // 当缓冲区不足一个头部（7字节）时，从文件中读取一个新块（32KB）
                buffer_ = {};
                // 读取一个块的内容到 buffer_.
                // backing_store_ 仅作为函数内部读取时的临时缓冲区，最终还是将数据放入 buffer_
                Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
                end_of_buffer_offset_ += buffer_.size();
                if (!status.ok()) {
                    buffer_ = {};
                    ReportDrop(kBlockSize, status);
                    eof_ = true;
                    return kEof;
                } else if (buffer_.size() < kBlockSize) {
                    eof_ = true;
                }
                continue;
            } else {
                buffer_ = {};
                return kEof;
            }
        }

        // 解析头部
        const char* header_ptr = buffer_.data();  // header_ptr指的是记录的首指针，不是仅头部的内容
        const uint32_t a = static_cast<uint32_t>(header_ptr[4]) & 0xff;
        const uint32_t b = static_cast<uint32_t>(header_ptr[5]) & 0xff;
        const unsigned int type = header_ptr[6];
        const uint32_t length = a | (b << 8);
        if (kHeaderSize + length > buffer_.size()) {
            size_t drop_size = buffer_.size();
            buffer_ = {};
            if (!eof_) {
                ReportCorruption(drop_size, "bad record length");
                return kBadRecord;
            }
            return kEof;
        }

        if (type == kZeroType && length == 0) {
            buffer_ = {};
            return kBadRecord;
        }

        if (checksum_) {
            uint32_t expected_crc = DecodeFixed32(header_ptr);
            uint32_t actual_crc = crc32c::Crc32c(header_ptr + 6, 1 + length);
            if (actual_crc != expected_crc) {
                ErrorLog << "actual_crc != expected_crc";
                size_t drop_size = buffer_.size();
                buffer_ = {};
                ReportCorruption(drop_size, "checksum mismatch");
                return kBadRecord;
            }
        }

        buffer_.remove_prefix(kHeaderSize + length);

        // 跳过 initial_offset_ 之前的记录
        if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_) {
            *result = {};
            return kBadRecord;
        }

        // 将数据部分放入 result
        *result = std::string_view(header_ptr + kHeaderSize, length);
        return type;
    }
}

/**
 * 读取逻辑记录
 * 负责组装跨块的分片记录
 * 处理流程：
 *  1. 首次读取时调用 SkipToInitialBlock() 定位到起始块
 *  2. 循环调用 ReadPhysicalRecord() 读取物理记录
 *  3. 根据记录类型组装逻辑记录：
 *      kFullType：直接返回
 *      kFirstType -> kMiddleType -> kLastType：使用 scratch 缓冲区拼装
 *  4. 处理异常情况（损坏、不完整记录等）
 */
bool Reader::ReadRecord(std::string_view* record, std::string* scratch) {
    // std::cout << "[ReadRecord] start to work" << std::endl;
    if (last_record_offset_ < initial_offset_) {
        // std::cout << "[ReadRecord] last_record_offset_ < initial_offset_" << std::endl;
        if (!SkipToInitialBlock()) {
            // std::cout << "[ReadRecord] !SkipToInitialBlock()" << std::endl;
            return false;
        }
    }
    // std::cout << "[ReadRecord] will clear" << std::endl;
    scratch->clear();
    // std::cout << "[ReadRecord] scratch->clear() over" << std::endl;
    *record = {};
    // std::cout << "[ReadRecord] *record = {} over" << std::endl;
    // std::cout << "[ReadRecord] clear over" << std::endl;
    bool in_fragmented_record = false;
    // 正在读取的逻辑记录的记录偏移量
    uint64_t prospective_record_offset = 0;

    std::string_view fragment;
    // std::cout << "[ReadRecord] will into while" << std::endl;
    while (true) {
        const unsigned int record_type = ReadPhysicalRecord(&fragment);

        uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

        if (resyncing_) {
            if (record_type == kMiddleType) {
                continue;
            } else if (record_type == kLastType) {
                resyncing_ = false;
                continue;
            } else {
                resyncing_ = false;
            }
        }

        // std::cout << "[ReadRecord] will switch" << std::endl;
        switch (record_type) {
            case kFullType:
                // std::cout << "[ReadRecord] case kFullType" << std::endl;
                // 直接返回完整记录
                if (in_fragmented_record) {
                    if (!scratch->empty()) {
                        ReportCorruption(scratch->size(), "partial record without end(1)");
                    }
                }
                prospective_record_offset = physical_record_offset;
                scratch->clear();
                *record = fragment;
                last_record_offset_ = prospective_record_offset;
                return true;

            case kFirstType:
                // std::cout << "[ReadRecord] case kFirstType" << std::endl;
                // 开始拼装跨块记录
                if (in_fragmented_record) {
                    if (!scratch->empty()) {
                        ReportCorruption(scratch->size(), "partial record without end(2)");
                    }
                }
                prospective_record_offset = physical_record_offset;
                scratch->assign(fragment.data(), fragment.size());
                in_fragmented_record = true;
                break;

            case kMiddleType:
                // std::cout << "[ReadRecord] case kMiddleType" << std::endl;
                // 追加中间片段
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "missing start of fragment record(1)");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                }
                break;

            case kLastType:
                // std::cout << "[ReadRecord] case kLastType" << std::endl;
                // 完成拼装并返回
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                    *record = std::string_view(*scratch);
                    last_record_offset_ = prospective_record_offset;
                    return true;
                }
                break;

            case kEof:
                // std::cout << "[ReadRecord] case kEof" << std::endl;
                if (in_fragmented_record) {
                    // 这可能是writer在写入某个物理记录后在完成下一个物理记录之前立即死亡
                    // 不视为损坏，只需忽略整个逻辑记录即可
                    scratch->clear();
                }
                return false;

            case kBadRecord:
                // std::cout << "[ReadRecord] case kBadRecord" << std::endl;
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "error in middle of record");
                    in_fragmented_record = false;
                    scratch->clear();
                }
                break;

            default: {
                // std::cout << "[ReadRecord] case default" << std::endl;
                std::string msg = std::format("unknown record type {}", record_type);
                ReportCorruption((fragment.size() + (in_fragmented_record ? scratch->size() : 0)), msg.c_str());
                in_fragmented_record = false;
                scratch->clear();
                break;
            }
        }
    }
    // std::cout << "[ReadRecord] while over" << std::endl;

    return false;
}

}  // namespace delta