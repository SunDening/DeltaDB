#include "version_edit.h"
#include "coding.h"
#include "version_set.h"

namespace delta {

// ============================================================================
// Tag 枚举：序列化字段的标签号
// ============================================================================
// 这些标签号被写入磁盘，永远不能修改（保证向后兼容）
// 每个 tag 标识一个字段的开始，解码时根据 tag 判断字段类型
// ============================================================================
enum Tag {
    kComparator = 1,      // 比较器名称]
    kWalNumber = 2,       // WAL 日志编号
    kNextSSTNumber = 3,   // 下一个 SSTable 文件编号
    kLastSequence = 4,    // 最后一个序列号
    kCompactPointer = 5,  // Compaction 进度指针
    kDeletedSST = 6,      // 删除的文件
    kNewSST = 7,          // 新增的文件
    // 8 was used for large value refs (已废弃)
    kPrevWalNumber = 9  // 前一个 WAL 日志编号（用于日志切换）
};

void VersionEdit::Clear() {
    comparator_.clear();
    wal_number_ = 0;
    prev_wal_number_ = 0;
    last_sequence_ = 0;
    next_sst_number_ = 0;
    has_comparator_ = false;
    has_wal_number_ = false;
    has_prev_wal_number_ = false;
    has_next_sst_number_ = false;
    has_last_sequence_ = false;
    compact_pointers_.clear();
    deleted_ssts_.clear();
    new_ssts_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
    if (has_comparator_) {
        PutVarint32(dst, kComparator);
        PutLengthPrefixedSlice(dst, comparator_);
    }
    if (has_wal_number_) {
        PutVarint32(dst, kWalNumber);
        PutVarint64(dst, wal_number_);
    }
    if (has_prev_wal_number_) {
        PutVarint32(dst, kPrevWalNumber);
        PutVarint64(dst, prev_wal_number_);
    }
    if (has_next_sst_number_) {
        PutVarint32(dst, kNextSSTNumber);
        PutVarint64(dst, next_sst_number_);
    }
    if (has_last_sequence_) {
        PutVarint32(dst, kLastSequence);
        PutVarint64(dst, last_sequence_);
    }

    // --- 编码 Compaction 指针数组 ---
    for (size_t i = 0; i < compact_pointers_.size(); i++) {
        PutVarint32(dst, kCompactPointer);
        PutVarint32(dst, compact_pointers_[i].first);  // level
        PutLengthPrefixedSlice(dst, compact_pointers_[i].second.Encode());
    }

    // --- 编码删除文件集合 ---
    for (const auto& deleted_file_kvp : deleted_ssts_) {
        PutVarint32(dst, kDeletedSST);
        PutVarint32(dst, deleted_file_kvp.first);   // level
        PutVarint64(dst, deleted_file_kvp.second);  // file number
    }

    // --- 编码新增文件列表 ---
    for (size_t i = 0; i < new_ssts_.size(); i++) {
        const SSTMetaData& sst = new_ssts_[i].second;
        PutVarint32(dst, kNewSST);
        PutVarint32(dst, new_ssts_[i].first);  // level
        PutVarint64(dst, sst.sst_number);
        PutVarint64(dst, sst.sst_size);
        PutLengthPrefixedSlice(dst, sst.smallest_key.Encode());
        PutLengthPrefixedSlice(dst, sst.largest_key.Encode());
    }
}

/**
 * @brief 辅助函数：解码 InternalKey
 * @param input：输入数据（会修改，前进读取位置）
 * @param dst：输出的 InternalKey
 * @return true：解码成功；false：解码失败
 */
static bool GetInternalKey(std::string_view* input, InternalKey* dst) {
    std::string_view str;
    // 先读取长度前缀的 slice，再解码 InternalKey
    if (GetLengthPrefixedSlice(input, &str)) {
        return dst->DecodeFrom(str);
    } else {
        return false;
    }
}

/**
 * @brief 辅助函数，解码 level
 */
static bool GetLevel(std::string_view* input, int* level) {
    uint32_t v;
    if (GetVarint32(input, &v) && static_cast<int>(v) < gDBConfig->num_levels) {
        *level = static_cast<int>(v);
        return true;
    } else {
        return false;
    }
}

Status VersionEdit::DecodeFrom(const std::string_view& src) {
    // 清空自身，准备接收数据
    Clear();

    std::string_view input = src;  // 可修改的输入视图
    const char* msg = nullptr;     // 错误消息（nullptr 表示无错误）
    uint32_t tag;                  // 当前读取的 tag

    // 临时变量，用于解析各个字段
    int level;
    uint64_t number;
    SSTMetaData sst;
    std::string_view str;
    InternalKey key;

    // 循环读取 tag 并解码对应字段
    while (msg == nullptr && GetVarint32(&input, &tag)) {
        switch (tag) {
            case kComparator:
                if (GetLengthPrefixedSlice(&input, &str)) {
                    comparator_ = str;
                    has_comparator_ = true;
                } else {
                    msg = "comparator name";
                }
                break;

            case kWalNumber:
                if (GetVarint64(&input, &wal_number_)) {
                    has_wal_number_ = true;
                } else {
                    msg = "log number";
                }
                break;

            case kPrevWalNumber:
                if (GetVarint64(&input, &prev_wal_number_)) {
                    has_prev_wal_number_ = true;
                } else {
                    msg = "previous log number";
                }
                break;

            case kNextSSTNumber:
                if (GetVarint64(&input, &next_sst_number_)) {
                    has_next_sst_number_ = true;
                } else {
                    msg = "next file number";
                }
                break;

            case kLastSequence:
                if (GetVarint64(&input, &last_sequence_)) {
                    has_last_sequence_ = true;
                } else {
                    msg = "last sequence number";
                }
                break;

            case kCompactPointer:
                if (GetLevel(&input, &level) && GetInternalKey(&input, &key)) {
                    compact_pointers_.push_back(std::make_pair(level, key));
                } else {
                    msg = "compaction pointer";
                }
                break;

            case kDeletedSST:
                if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
                    deleted_ssts_.insert(std::make_pair(level, number));
                } else {
                    msg = "deleted file";
                }
                break;

            case kNewSST:
                if (GetLevel(&input, &level) && GetVarint64(&input, &sst.sst_number) &&
                    GetVarint64(&input, &sst.sst_size) && GetInternalKey(&input, &sst.smallest_key) &&
                    GetInternalKey(&input, &sst.largest_key)) {
                    new_ssts_.push_back(std::make_pair(level, sst));
                } else {
                    msg = "new-file entry";
                }
                break;

            default:
                msg = "unknown tag";
                break;
        }
    }

    if (msg == nullptr && !input.empty()) {
        msg = "invalid tag";
    }

    Status result;
    if (msg != nullptr) {
        result = Status::Corruption("VersionEdit", msg);
    }
    return result;
}

}  // namespace delta