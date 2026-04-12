#pragma once

#include <set>
#include <utility>
#include <vector>

#include <deltadb/utils/dbformat.h>

namespace delta {

/**
 * @brief 定义了数据库版本管理的核心数据结构，用于描述数据库版本的变化。
 * LevelDB 使用 LSM-Tree 结构，数据分布在多个 SSTable 文件中。随着写入和 Compaction 的进行，文件集合不断变化。
 * LevelDB 用 Version 来表示某一时刻的文件集合快照，用 VersionEdit 来描述版本之间的变化。
 *
 * Version (版本) = 某一时刻所有 SSTable 文件的集合
           ↓
    VersionEdit (版本编辑) = 描述如何从一个 Version 变到下一个 Version
           ↓
    Manifest 文件 = 存储所有 VersionEdit 的持久化日志
 */

class VersionSet;

/**
 * SST 文件元数据
 */
struct SSTMetaData {
    int refs;                  // 引用计数
    int allowed_seeks;         // 允许 Seek 次数，触发
    uint64_t sst_number;       // 文件编号（对应 .sst 文件名）
    uint64_t sst_size;         // 文件大小（字节）
    InternalKey smallest_key;  // 文件中最小的 key
    InternalKey largest_key;   // 文件中最大的 key

    SSTMetaData() : refs(0), allowed_seeks(1 << 30), sst_size(0) {}
};

/**
 * @brief 版本编辑，记录变更
 * 记录的内容包括：
 *  1. 元信息：comparator、log_number、next_file_number 等
 *  2. 文件变化：deleted_files_（删除的文件）、new_files_（新增的文件）
 *  3. Compaction 进度：compact_pointers_（记录每个 level 的 compaction 位置）
 *
 * 使用场景：
 *  1. Compaction 完成后，生成 VersionEdit 描述文件变化
 *  2. 写入新数据时，更新 log_number、next_file_number 等
 *  3. 重启时，从 Manifest 文件读取 VersionEdit 序列恢复状态
 */
class VersionEdit {
   private:
    friend class VersionSet;

    // 已删除的文件集合：元素为 (level, file_number)
    typedef std::set<std::pair<int, uint64_t>> DeletedSSTSet;

    std::string comparator_;        // 比较器名称
    uint64_t wal_number_;           // 当前 日志编号
    uint64_t prev_wal_number_;      // 前一个 日志编号（用于切换日志）
    uint64_t next_sst_number_;      // 下一个 SSTable 文件编号（全局递增）
    SequenceNumber last_sequence_;  // 最后一个使用的序列号

    bool has_comparator_;
    bool has_wal_number_;
    bool has_prev_wal_number_;
    bool has_next_sst_number_;
    bool has_last_sequence_;

    // --------------------------------------------------------------------------
    // 变更内容
    // --------------------------------------------------------------------------

    // Compaction 指针：记录每个 level 的 compaction 进度
    // 重启后可以从该位置继续 compaction
    std::vector<std::pair<int, InternalKey>> compact_pointers_;

    // 新增文件列表：(level, SSTMetaData)
    std::vector<std::pair<int, SSTMetaData>> new_ssts_;

    // 已删除文件集合：(level, file_number)
    DeletedSSTSet deleted_ssts_;

   public:
    VersionEdit() { Clear(); }
    ~VersionEdit() = default;

    /**
     * @brief  清空所有字段
     */
    void Clear();

    // --------------------------------------------------------------------------
    // 设置元信息
    // --------------------------------------------------------------------------

    void SetComparatorName(const std::string_view& name) {
        has_comparator_ = true;
        comparator_ = name.data();
    }

    void SetWalNumber(uint64_t num) {
        has_wal_number_ = true;
        wal_number_ = num;
    }

    void SetPrevWalNumber(uint64_t num) {
        has_prev_wal_number_ = true;
        prev_wal_number_ = num;
    }

    void SetNextSSTNumber(uint64_t num) {
        has_next_sst_number_ = true;
        next_sst_number_ = num;
    }

    void SetNextSST(uint64_t num) {
        has_next_sst_number_ = true;
        next_sst_number_ = num;
    }

    void SetLastSequence(SequenceNumber seq) {
        has_last_sequence_ = true;
        last_sequence_ = seq;
    }

    /**
     * @brief 追加 compact 进度到compact_pointers_
     */
    void AppendCompactPointer(int level, const InternalKey& key) {
        compact_pointers_.push_back(std::make_pair(level, key));
    }

    /**
     * @brief 添加一个 SSTable 文件到指定 level
     * @param level：文件所属的层级
     * @param sst_number：文件编号
     * @param sst_size：文件大小
     * @param smallest_key：文件中最小的 key
     * @param largest_key：文件中最大的 key
     */
    void AddSST(int level, uint64_t sst_number, uint64_t sst_size, const InternalKey& smallest_key,
                const InternalKey& largest_key) {
        SSTMetaData meta;
        meta.sst_number = sst_number;
        meta.sst_size = sst_size;
        meta.smallest_key = smallest_key;
        meta.largest_key = largest_key;
        new_ssts_.push_back(std::make_pair(level, meta));
    }

    /**
     * @brief 从指定 level 删除一个 SSTable 文件，实际上是添加到已删除的文件集合
     * @param level：文件所属的层级
     * @param sst_number：文件编号
     */
    void RemoveSST(int level, uint64_t sst_number) { deleted_ssts_.insert(std::make_pair(level, sst_number)); }

    /**
     * @brief 将 VersionEdit 序列化为二进制格式，追加到 dst
     * @param dst：输出字符串，序列化数据追加到末尾
     * 编码格式：[tag][value][tag][value]...只编码已设置的字段（has_* == true）
     */
    void EncodeTo(std::string* dst) const;

    /**
     * @brief 从 src 反序列化 VersionEdit
     */
    Status DecodeFrom(const std::string_view& src);
};

}  // namespace delta