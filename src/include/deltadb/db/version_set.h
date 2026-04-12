#pragma once

#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <deltadb/db/version_edit.h>
#include <deltadb/utils/config.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/wal/wal_writer.h>

namespace delta {

extern delta::Config::ptr gDBConfig;

/**
 * 版本集合，定义了版本管理系统的核心组件，负责管理数据库的所有版本
 *
 * 核心概念：
 *  - DBImpl 由一组 Version 组成，最新的 Version 称为 "current"
 *  - 旧 Version 会被保留，为活跃的 Iterator 提供一致性视图
 *  - 每个 Version 按 level 组织一组 SSTable 文件
 *  - 所有 Version 由 VersionSet 统一管理
 *
 *
    ┌─────────────────────────────────────────────────────────────────┐
    │ VersionSet (版本集合)                                           │
    │ ┌─────────────────────────────────────────────────────────────┐ │
    │ │ Version (版本 0) → Version (版本 1) → Version (版本 2)      │ │
    │ │   (旧)               (旧)               (current/最新)       │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    └─────────────────────────────────────────────────────────────────┘

    每个 Version 包含：
      Level 0: [File 1] [File 2] [File 3]  ← SSTable 文件集合
      Level 1: [File 4] [File 5]
      Level 2: [File 6]
      ...
 *
 */

class Writer;        // MANIFEST 文件的WAL写入器
class Compaction;    // Compaction 任务描述
class Iterator;      // 迭代器
class MemTable;      // 内存表
class SSTBuilder;    // SSTable 构建器
class SSTCache;      // SSTable 缓存
class Version;       // 版本
class VersionSet;    // 版本集合
class WritableFile;  // 可写文件

/**
 * @brief 查找文件，在有序文件列表中找到第一个满足 sst_metas[i]->largest_key >= key 的文件索引
 * @param inter_comp：内部键比较器
 * @return 满足条件的最小索引 / sst_metas.size()（如果没有这样的文件）
 */
int FindFile(const InternalKeyComparator& inter_comp, const std::vector<SSTMetaData*>& sst_metas,
             const std::string_view& key);

/**
 * @brief 判断某个 level 的 ssts 中是否有文件与用户 key 范围重叠
 * @param inter_camp：内部键比较器
 * @param disjoint_sorted_ssts：如果为true，ssts_metas包含不相交的有序范围
 * @param smallest_user_key：范围下界（nullptr 表示小于所有 key）
 * @param largest_user_key：范围上界（nullptr 表示大于所有 key）
 */
bool IsSomeFileOverlapsRange(const InternalKeyComparator& inter_camp, bool disjoint_sorted_ssts,
                             const std::vector<SSTMetaData*>& sst_metas, const std::string_view* smallest_user_key,
                             const std::string_view* largest_user_key);

/**
 * @brief 版本，表示数据库在某一时刻的快照（所有 SSTable 文件的集合）
 * 数据结构：
 *  - 双向链表：所有 Version 通过 next_/prev_ 连接成环
 *  - 文件列表：ssts_[level] 存储该 level 的所有文件
 *  - 统计信息：compaction_score_ 等用于决策 compaction
 */
class Version {
   private:
    friend class Compaction;
    friend class VersionSet;

    class LevelSSTNumIterator;  // Level 文件编号迭代器

    VersionSet* vset_;  // 所属的 VersionSet
    Version* next_;     // 链表下一个 Version
    Version* prev_;     // 链表上一个 Version
    int refs_;          // 活跃引用计数

    std::vector<SSTMetaData*> ssts_[7];  // 每个 level 的文件列表

    // 基于 Seek 统计需要 compact 的文件
    SSTMetaData* sst_to_compact_;
    int sst_to_compact_level_;

    double compaction_score_;  // 需要 compact 的 level 的压实分数
    int compaction_level_;     // 下一个需要 compact 的 level

    explicit Version(VersionSet* vset)
        : vset_(vset),
          next_(this),
          prev_(this),
          refs_(0),
          sst_to_compact_(nullptr),
          sst_to_compact_level_(-1),
          compaction_score_(-1),
          compaction_level_(-1) {}

    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    ~Version();

    /**
     * @brief 创建连接迭代器：将指定 level 的所有文件迭代器连接成一个
     */
    Iterator* NewConcatenateIterator(const ReadOptions&, int level) const;

    /**
     * @brief 对每个与 user_key 重叠的文件调用 func(arg, level, f)
     * 从最新到最旧排序，如果 func 返回 false 则停止调用
     */
    void CallForEachOverlapSST(std::string_view user_key, std::string_view internal_key, void* arg,
                               bool (*func)(void*, int, SSTMetaData*));

   public:
    // 用于记录 Seek 统计信息
    struct GetStats {
        SSTMetaData* seek_file;  // 触发 Seek 的文件
        int seek_file_level;     // 文件所在 level
    };

    /**
     * @brief 为这个 Version 的所有文件创建迭代器，添加到 *iters
     */
    void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

    /**
     * @brief 在 Version 中查找 key，找到则存入 *val 并返回 OK
     * @param stats：输出参数，记录 Seek 统计（用于触发基于读压力的 compaction）
     */
    Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

    /**
     * @brief 将 "stats" 加入当前状态，返回是否需要触发新的 compaction
     */
    bool UpdateStats(const GetStats& stats);

    /**
     * @brief 记录在指定 internal key 处读取的字节样本
     * @return 返回是否需要触发新的 compaction
     */
    bool RecordReadSample(std::string_view internal_key);

    /**
     * @brief 引用计数管理，管理 Version 的生命周期
     * 活跃的 Iterator 会持有 Version 的引用。
     * 引用计数为 0 时，Version 会被删除。
     * 确保 Iterator 读取过程中 Version 不会消失。
     */
    void Ref();

    void Unref();

    /**
     * @brief 找出 level 层中与 [*begin, *end] 范围重叠的所有文件
     * @param begin：范围下界（nullptr 表示在所有 key 之前）
     * @param end：范围上界（nullptr 表示在所有 key 之后）
     * @param ssts：输出参数，存储找到的文件
     */
    void GetOverlapSSTs(int level, const InternalKey* begin, const InternalKey* end, std::vector<SSTMetaData*>* ssts);

    /**
     * @brief 判断指定 level 是否有文件与 [*smallest_user_key, *largest_user_key] 重叠
     * @param smallest_user_key：范围下界（nullptr 表示小于所有 DB 的 key）
     * @param largest_user_key：范围上界（nullptr 表示大于所有 DB 的 key）
     */
    bool IsOverlapInLevel(int level, const std::string_view* smallest_user_key,
                          const std::string_view* largest_user_key);

    /**
     * @brief 为 MemTable compaction 的输出选择合适的 level
     * @param smallest_user_key：输出文件的 key 范围下界
     * @param largest_user_key：输出文件的 key 范围上界
     * @return 应该放置新文件的 level（通常是 0，如果有重叠则提升到更高层）
     */
    int PickLevelForMemTableCompactionOutput(const std::string_view& smallest_user_key,
                                             const std::string_view& largest_user_key);

    /**
     * @brief 返回指定 level 的文件数量
     */
    int SSTNum(int level) const { return ssts_[level].size(); }
};

/**
 * @brief 版本集合，管理所有 Version 的核心类
 * 职责：
 *  1. 维护 Version 链表（current 指向最新版本）
 *  2. 持久化 VersionEdit 到 MANIFEST 文件
 *  3. 从 MANIFEST 恢复状态
 *  4. 选择 Compaction 任务
 *  5. 分配文件编号
 */
class VersionSet {
   private:
    class Builder;  // VersionSet 构建器

    friend class Compaction;
    friend class Version;

    const std::string dbname_;                // 数据库路径
    SSTCache* const sst_cache_;               // sst 缓存
    const InternalKeyComparator inter_comp_;  // InternalKey 比较器

    uint64_t next_file_number_;  // 下一个文件编号（全局递增）
    uint64_t manifest_number_;   // MANIFEST 文件编号
    uint64_t last_sequence_;     // 最后一个序列号
    uint64_t log_number_;        // 当前 log 编号
    uint64_t prev_log_number_;   // 前一个 log 编号

    WritableFile* manifest_;  // MANIFEST 文件
    Writer* manifest_log_;    // MANIFEST 日志写入器

    Version dummy_versions_;  // 循环双向链表头（哨兵节点）
    Version* current_;        // == dummy_versions_.prev_（最新版本）

    std::string compact_pointer_[7];  // 每个 level 的 compaction 起始指针

    /**
     * @brief 复用 MANIFEST 文件
     */
    bool ReuseManifest(const std::string& manifest_name, const std::string& manifest_base);

    /**
     * @brief 计算 Version 的 compaction 分数
     * @param v：目标 Version
     * 策略：
     *  - Level 0: 分数 = 文件数 / kL0_CompactionTrigger (默认 4)
     *  - Level > 0: 分数 = 当前大小 / 该层最大大小
     * 选择分数最高的 level 作为下一个 compaction 目标
     */
    void Finalize(Version* v);

    /**
     * @brief 获取输入文件的 key 范围
     * @param smallest_key：范围下限
     * @param largest_key：范围上限
     */
    void GetRange(const std::vector<SSTMetaData*>& inputs, InternalKey* smallest_key, InternalKey* largest_key);

    /**
     * @brief 获取两组输入文件的合并 key 范围
     * @param smallest_key：范围下限
     * @param largest_key：范围上限
     */
    void GetRange2(const std::vector<SSTMetaData*>& inputs1, const std::vector<SSTMetaData*>& inputs2,
                   InternalKey* smallest_key, InternalKey* largest_key);

    /**
     * @brief 为 Compaction 设置另一组输入文件
     */
    void SetupOtherInputs(Compaction* c);

    /**
     * @brief 将当前内容保存到 *log
     */
    Status WriteSnapshot(Writer* log);

    /**
     * @brief 添加 Version 到链表末尾
     */
    void AppendVersion(Version* v);

   public:
    VersionSet(const std::string& dbname, SSTCache* sst_cache, const InternalKeyComparator*);

    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    ~VersionSet();

    /**
     * @brief 将 *edit 应用到当前 Version，生成新的描述符
     *  1. 写入 MANIFEST 文件（持久化）
     *  2. 安装为新的 current version
     */
    Status LogAndApply(VersionEdit* edit, std::mutex* mtx);

    /**
     * @brief 从 MANIFEST 文件恢复最后一个保存的描述符
     * @param save_manifest：输出参数，是否需要保存新的 manifest
     */
    Status Recover(bool* save_manifest);

    /**
     * @brief 返回当前版本（最新版本）
     */
    Version* current() const { return current_; }

    /**
     * @brief 返回当前 MANIFEST 编号
     */
    uint64_t ManifestFileNumber() const { return manifest_number_; }

    /**
     * @brief 分配并返回新的文件编号（全局递增）
     */
    uint64_t NewFileNumber() { return next_file_number_++; }

    /**
     * @brief 重用文件编号
     */
    void ReuseFileNumber(uint64_t file_number) {
        if (next_file_number_ == file_number + 1) {
            next_file_number_ = file_number;
        }
    }

    /**
     * @brief 返回指定 level 的 SST 文件数量
     */
    int SSTNumOfLevel(int level) const;

    /**
     * @brief 返回指定 level 的所有文件的总大小（字节）
     */
    int64_t SSTBytesOfLevel(int level) const;

    /**
     * @brief 返回最后一个序列号
     */
    uint64_t LastSequence() const { return last_sequence_; }

    /**
     * @brief 设置最后一个序列号（只能递增）
     */
    void SetLastSequence(uint64_t s) {
        assert(s >= last_sequence_);
        last_sequence_ = s;
    }

    /**
     * @brief 标记指定文件编号为已使用
     */
    void MarkFileNumberAsUsed(uint64_t number);

    /**
     * @brief 返回当前 log 文件编号
     */
    uint64_t LogNumber() const { return log_number_; }

    /**
     * @brief 返回正在 compact 的日志文件编号，如果没有则为 0
     */
    uint64_t PrevWalNumber() const { return prev_log_number_; }

    /**
     * @brief 选择一个新的 compaction 任务（包括 level 和输入文件）
     * @return 如果不需要 compaction，返回 nullptr；否则返回堆分配的 Compaction 对象（调用者负责删除）
     */
    Compaction* PickCompaction();

    /**
     * @brief 在指定 level 的 [begin, end] 范围内选择需要进行压缩的文件并创建 compaction 对象
     * @return 如果该 level 没有文件与范围重叠，返回 nullptr；否则返回堆分配的 Compaction 对象（调用者负责删除）
     */
    Compaction* CompactRange(int level, const InternalKey* begin, const InternalKey* end);

    /**
     * @brief 返回 level >= 1 的文件在下一层的最大重叠字节数
     */
    int64_t MaxNextLevelOverlapBytes();

    /**
     * @brief 为 compaction 的输入文件创建迭代器
     */
    Iterator* MakeInputIterator(Compaction* c);

    /**
     * @brief 是否需要 Compaction
     */
    bool IsNeedCompaction() const {
        Version* v = current_;
        return (v->compaction_score_ >= 1) || (v->sst_to_compact_ != nullptr);
    }

    /**
     * @brief 添加活跃文件，将所有活跃 Version 中的文件添加到 *live
     */
    void AddLiveFiles(std::set<uint64_t>* live);

    /**
     * @brief 返回 "key" 在数据库中的近似偏移量（截至版本 "v"）
     * 在 LevelDB 的合并迭代器（Merging Iterator）遍历顺序中，到达这个 key 之前需要扫描的数据量。
     */
    uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

    // Level 摘要存储（用于 LevelSummary）
    struct LevelSummaryStorage {
        char buffer[100];
    };
    /**
     * @brief 返回每层文件数量的人类可读摘要
     */
    const char* LevelSummary(LevelSummaryStorage* scratch) const;
};

/**
 * @brief 封装一次 compaction 任务的完整信息
 * 数据结构：
 *  - inputs_[0]: level_ 层的输入文件
 *  - inputs_[1]: level_+1 层的输入文件（与 inputs_[0] 重叠的文件）
 *  - grandparents_: level_+2 层的文件（用于限制输出大小）
 *
 */
class Compaction {
   private:
    friend class Version;
    friend class VersionSet;

    int level_;                      // 正在 compact 的 level
    uint64_t max_output_file_size_;  // 输出文件最大大小
    Version* input_version_;         // 输入 Version
    VersionEdit edit_;               // 此 compaction 生成的版本编辑

    // 两组输入文件
    // inputs_[0] = level_ 层的文件
    // inputs_[1] = level_+1 层的文件
    std::vector<SSTMetaData*> inputs_[2];

    // --- 用于检查与 grandparent 文件重叠的状态 ---
    std::vector<SSTMetaData*> grandparents_;  // grandparent 文件（level_+2）
    size_t grandparent_index_;                // 在 grandparent_starts_ 中的索引
    bool seen_key_;                           // 是否已见过某个输出 key
    int64_t overlapped_bytes_;                // 当前输出与 grandparent 文件的重叠字节数

    // --- 用于实现 IsBaseLevelForKey 的状态 ---
    // level_ptrs_ 存储 input_version_->levels_ 的索引
    // 状态：对于所有 L >= level_ + 2 的层，我们定位在某个文件范围
    size_t level_ptrs_[7];

    Compaction(int level);

   public:
    ~Compaction();

    /**
     * @brief 返回正在 compact 的 level
     */
    int level() const { return level_; }

    /**
     * @brief 返回此 compaction 生成的 VersionEdit
     */
    VersionEdit* edit() { return &edit_; }

    /**
     * @brief 某层输入文件的数量
     * @param "which" 必须是 0 或 1。which=0: level_ 层的输入文件；which=1: level_+1 层的输入文件
     */
    int input_files_num(int which) const { return inputs_[which].size(); }

    /**
     * @brief 返回 "level()+which" 层的第 i 个输入文件
     */
    SSTMetaData* get_input_file(int which, int i) const { return inputs_[which][i]; }

    /**
     * @brief Compaction 输出文件的最大大小
     */
    uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

    /**
     * @brief 判断是否可以通过简单移动单个文件到下一层来实现 compaction（不需要合并或拆分）
     */
    bool IsJustMove() const;

    /**
     * @brief compaction 完成后，将此 compaction 的所有输入文件作为删除操作添加到 *edit
     */
    void AddInputDeletions(VersionEdit* edit);

    /**
     * @brief 判断 compaction 在 "level_+1" 层产生的 key 是否在更高层不存在（实际上是看是否被更高层某个sst的范围所包含）
     * @return true  = level_+1 之后的层没有该 key 的数据；false = 更高层可能有该 key 的数据
     */
    bool IsBaseLevelForKey(const std::string_view& user_key);

    /**
     * @brief 判断在处理 "internal_key" 之前是否应该停止构建当前输出文件。
     * 用于限制输出文件与 grandparent 层（level_+2）的重叠大小。
     * @param internal_key：即将处理的 internal key
     * @return true：停止当前输出，开始新文件；false：继续构建当前文件
     */
    bool ShouldStopBefore(const std::string_view& internal_key);

    /**
     * @brief compaction 成功后，释放输入 Version 的引用
     */
    void ReleaseInputs();
};

}  // namespace delta