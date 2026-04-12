#include <algorithm>
#include <cstdint>

#include <deltadb/db/filename.h>
#include <deltadb/db/memtable.h>
#include <deltadb/db/version_set.h>
#include <deltadb/table/iter_merger.h>
#include <deltadb/table/sst_cache.h>
#include <deltadb/table/two_level_iterator.h>
#include <deltadb/utils/coding.h>
#include <deltadb/wal/wal_reader.h>
#include <deltadb/wal/wal_writer.h>

namespace delta {

// ============================================================================
// 辅助函数
// ============================================================================

// 目标文件大小 默认 2MB
static size_t TargetFileSize() { return gDBConfig->max_file_size; }

// 最大 grandparent 重叠字节数 默认 20MB
static int64_t MaxGrandParentOverlapBytes() { return 10 * TargetFileSize(); }

// 扩展 Compaction 字节限制，默认 50MB。限制 compaction 输入文件的总大小，避免一次性 compact 太多数据
static int64_t ExpandedCompactionByteSizeLimit() { return 25 * TargetFileSize(); }

/**
 * @brief 每层最大字节数
 * @param 层数：0~6
 * @return level 0/1: 10MB，此后每层递增10倍.
 * Level 0 的结果实际上不使用，因为 level-0 的 compaction 阈值基于文件数量
 */
static double MaxBytesForLevel(int level) {
    double result = 10. * 1048576.0;  // 10MB
    while (level > 1) {
        result *= 10;
        level--;
    }
    return result;
}

/**
 * @brief 每层最大文件大小
 * @return TargetFileSize（默认 2MB）.所有 level 使用相同的目标文件大小
 */
static uint64_t MaxSSTSizeForLevel(int /*level*/) { return TargetFileSize(); }

/**
 * @brief 计算文件总大小
 */
static int64_t TotalSSTSize(const std::vector<SSTMetaData*>& ssts) {
    int64_t sum = 0;
    for (size_t i = 0; i < ssts.size(); i++) {
        sum += ssts[i]->sst_size;
    }
    return sum;
}

// ============================================================================

Version::~Version() {
    assert(refs_ == 0);

    // 从双向链表中移除
    prev_->next_ = next_;
    next_->prev_ = prev_;

    // 释放对所有文件的引用
    for (int level = 0; level < gDBConfig->num_levels; level++) {
        for (size_t i = 0; i < ssts_[level].size(); i++) {
            SSTMetaData* f = ssts_[level][i];
            assert(f->refs > 0);
            f->refs--;
            if (f->refs <= 0) {
                delete f;  // 引用计数为 0 时删除文件元数据
            }
        }
    }
}

/**
 * @brief 二分查找第一个满足 ssts[i]->largest >= key 的文件
 * @param inter_comp：InternalKey 比较器
 * @param ssts：有序且不重叠的文件列表
 * @param key：目标 key
 * @param 满足条件的最小索引 i / files.size()（如果没有这样的文件）
 */
int FindSST(const InternalKeyComparator& inter_comp, const std::vector<SSTMetaData*>& ssts,
            const std::string_view& key) {
    uint32_t left = 0;
    uint32_t right = ssts.size();
    while (left < right) {
        uint32_t mid = (left + right) / 2;
        const SSTMetaData* f = ssts[mid];
        if (inter_comp.Compare(f->largest_key.Encode(), key) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return right;
}

/**
 * @brief 判断 user_key 是否在文件 sst 之后
 */
static bool AfterSST(const Comparator* user_comp, const std::string_view* user_key, const SSTMetaData* sst) {
    return (user_key != nullptr && user_comp->Compare(*user_key, sst->largest_key.user_key()) > 0);
}

/**
 * @brief 判断 user_key 是否在 sst 之前
 */
static bool BeforeSST(const Comparator* user_comp, const std::string_view* user_key, const SSTMetaData* sst) {
    return (user_key != nullptr && user_comp->Compare(*user_key, sst->smallest_key.user_key()) < 0);
}

bool IsSomeFileOverlapsRange(const InternalKeyComparator& inter_camp, bool disjoint_sorted_ssts,
                             const std::vector<SSTMetaData*>& sst_metas, const std::string_view* smallest_user_key,
                             const std::string_view* largest_user_key) {
    const Comparator* user_comp = inter_camp.user_comparator();
    if (!disjoint_sorted_ssts) {
        // 文件可能重叠，需要检查所有文件
        for (size_t i = 0; i < sst_metas.size(); i++) {
            const SSTMetaData* sst = sst_metas[i];
            if (AfterSST(user_comp, smallest_user_key, sst) || BeforeSST(user_comp, largest_user_key, sst)) {
                // 范围要么在sst之前，要么在sst之后，总之与sst没有重叠
            } else {
                return true;  // 有重叠
            }
        }
        return false;
    }

    // 文件已排序且不相交，使用二分查找优化
    uint32_t index = 0;
    if (smallest_user_key != nullptr) {
        // 找到第一个 largest >= smallest_user_key 的文件
        InternalKey small_key(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        index = FindSST(inter_camp, sst_metas, small_key.Encode());
    }

    if (index >= sst_metas.size()) {
        return false;
    }

    return !BeforeSST(user_comp, largest_user_key, sst_metas[index]);
}

// ============================================================================
// Version::LevelFileNumIterator - Level 文件编号迭代器
// ============================================================================

/**
 * @brief Level 文件编号迭代器
 * 用途：
 *  - 为某个 level 的文件列表提供迭代器接口
 *  - key() = 文件的 largest key
 *  - value() = 文件编号 (8 字节) + 文件大小 (8 字节)
 */
class Version::LevelSSTNumIterator : public Iterator {
   private:
    const InternalKeyComparator inter_comp_;
    const std::vector<SSTMetaData*>* const sst_list_;
    uint32_t index_;

    mutable char value_buf_[16];  // 存储 value() 的缓冲区

   public:
    LevelSSTNumIterator(const InternalKeyComparator& inter_comp, const std::vector<SSTMetaData*>* sst_list)
        : inter_comp_(inter_comp), sst_list_(sst_list), index_(sst_list->size()) {}

    bool Valid() const override { return index_ < sst_list_->size(); }

    void Seek(const std::string_view& target) override { index_ = FindSST(inter_comp_, *sst_list_, target); }

    void SeekToFirst() override { index_ = 0; }
    void SeekToLast() override { index_ = sst_list_->empty() ? 0 : sst_list_->size() - 1; }
    void Next() override {
        assert(Valid());
        index_++;
    }
    void Prev() override {
        assert(Valid());
        if (index_ == 0) {
            index_ = sst_list_->size();  // 标记为无效
        } else {
            index_--;
        }
    }

    /**
     * @brief key() = 文件的 largest key
     */
    std::string_view key() const override {
        assert(Valid());
        return (*sst_list_)[index_]->largest_key.Encode();
    }

    /**
     * @brief value() = 文件编号 (8 字节) + 文件大小 (8 字节)
     */
    std::string_view value() const override {
        assert(Valid());
        EncodeFixed64(value_buf_, (*sst_list_)[index_]->sst_number);
        EncodeFixed64(value_buf_ + 8, (*sst_list_)[index_]->sst_size);
        return std::string_view(value_buf_, sizeof(value_buf_));
    }

    Status status() const override { return Status::OK(); }
};

static Iterator* GetSSTIterator(void* arg, const ReadOptions& options, const std::string_view& sst_value) {
    SSTCache* cache = reinterpret_cast<SSTCache*>(arg);
    if (sst_value.size() != 16) {
        return NewErrorIterator(Status::Corruption("FileReader invoked with unexpected value"));
    } else {
        return cache->NewIterator(options, DecodeFixed64(sst_value.data()), DecodeFixed64(sst_value.data() + 8));
    }
}

Iterator* Version::NewConcatenateIterator(const ReadOptions& options, int level) const {
    return NewTwoLevelIterator(new LevelSSTNumIterator(vset_->inter_comp_, &ssts_[level]), &GetSSTIterator,
                               vset_->sst_cache_, options);
}

void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
    // 因为它们可能范围重叠，每个文件单独创建迭代器
    for (size_t i = 0; i < ssts_[0].size(); i++) {
        iters->push_back(vset_->sst_cache_->NewIterator(options, ssts_[0][i]->sst_number, ssts_[0][i]->sst_size));
    }

    // 对于 Level > 0，使用连接迭代器顺序遍历不重叠的文件
    for (int level = 1; level < gDBConfig->num_levels; level++) {
        if (!ssts_[level].empty()) {
            iters->push_back(NewConcatenateIterator(options, level));
        }
    }
}

namespace {
enum SaverState {
    kNotFound,
    kFound,
    kDeleted,
    kCorrupt,
};

struct Saver {
    SaverState state;
    const Comparator* user_comp;
    std::string_view user_key;
    std::string* value;
};
}  // namespace

static void SaveValue(void* arg, const std::string_view& ikey, const std::string_view& v) {
    Saver* s = reinterpret_cast<Saver*>(arg);
    ParsedInternalKey parsed_key;
    if (!ParseInternalKey(ikey, &parsed_key)) {
        s->state = kCorrupt;
    } else {
        if (s->user_comp->Compare(parsed_key.user_key, s->user_key) == 0) {
            s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
            if (s->state == kFound) {
                s->value->assign(v.data(), v.size());
            }
        }
    }
}

static bool NewestFirst(SSTMetaData* a, SSTMetaData* b) { return a->sst_number > b->sst_number; }

void Version::CallForEachOverlapSST(std::string_view user_key, std::string_view internal_key, void* arg,
                                    bool (*func)(void*, int, SSTMetaData*)) {
    const Comparator* user_comp = vset_->inter_comp_.user_comparator();

    // 收集 Level 0 中范围包括 user_key 的 sst 文件
    std::vector<SSTMetaData*> tmp;
    tmp.reserve(ssts_[0].size());
    for (uint32_t i = 0; i < ssts_[0].size(); i++) {
        SSTMetaData* sst = ssts_[0][i];
        if (user_comp->Compare(user_key, sst->smallest_key.user_key()) >= 0 &&
            user_comp->Compare(user_key, sst->largest_key.user_key()) <= 0) {
            tmp.push_back(sst);
        }
    }

    if (!tmp.empty()) {
        // 将从 Level 0 中收集到的 sst 文件按从新到旧排序
        std::sort(tmp.begin(), tmp.end(), NewestFirst);
        for (uint32_t i = 0; i < tmp.size(); i++) {
            // 调用 func，直到返回false
            if (!(*func)(arg, 0, tmp[i])) {
                return;
            }
        }
    }

    // 其他层级：每个层级文件间 key 不重叠，使用 user_key 二分查找
    for (int level = 1; level < gDBConfig->num_levels; level++) {
        size_t sst_count = ssts_[level].size();
        if (sst_count == 0) continue;

        // 二分查找：找到第一个 largest_key.user_key() >= user_key 的文件
        uint32_t index = FindSST(vset_->inter_comp_, ssts_[level], internal_key);
        if (index < sst_count) {
            SSTMetaData* sst = ssts_[level][index];
            // 检查 user_key 是否在 [smallest_key, largest_key] 范围内
            if (user_comp->Compare(user_key, sst->smallest_key.user_key()) >= 0) {
                if (!(*func)(arg, level, sst)) {
                    return;
                }
            }
        }
    }
}

Status Version::Get(const ReadOptions& options, const LookupKey& key, std::string* val, GetStats* stats) {
    stats->seek_file = nullptr;
    stats->seek_file_level = -1;

    // 保存查找过程中的状态
    struct State {
        Saver saver;      // 保存回调函数的状态
        GetStats* stats;  // Seek 统计
        const ReadOptions* options;
        std::string_view ikey;        // Internal key
        SSTMetaData* last_file_read;  // 最后读取的文件
        int last_file_read_level;     // 最后读取的文件 level

        VersionSet* vset;
        Status s;
        bool found;  // 是否找到

        // 回调函数：对每个重叠的文件调用
        static bool Match(void* arg, int level, SSTMetaData* sst) {
            State* state = reinterpret_cast<State*>(arg);

            // 如果有超过一次 Seek，记录第一个文件用于触发 compaction
            if (state->stats->seek_file == nullptr && state->last_file_read != nullptr) {
                state->stats->seek_file = state->last_file_read;
                state->stats->seek_file_level = state->last_file_read_level;
            }

            state->last_file_read = sst;
            state->last_file_read_level = level;

            // 从 SSTCache 中获取数据
            state->s = state->vset->sst_cache_->Get(*state->options, sst->sst_number, sst->sst_size, state->ikey,
                                                    &state->saver, SaveValue);
            if (!state->s.ok()) {
                state->found = true;
                return false;
            }
            switch (state->saver.state) {
                case kNotFound:
                    return true;  // 继续搜索其他文件
                case kFound:
                    state->found = true;
                    return false;  // 找到，停止搜索
                case kDeleted:
                    return false;  // 遇到删除标记，停止搜索
                case kCorrupt:
                    state->s = Status::Corruption("corrupted key for ", state->saver.user_key);
                    state->found = true;
                    return false;
            }
            return false;
        }
    };

    // 初始化状态
    State state;
    state.found = false;
    state.stats = stats;
    state.last_file_read = nullptr;
    state.last_file_read_level = -1;

    state.options = &options;
    state.ikey = key.internal_key();
    state.vset = vset_;

    state.saver.state = kNotFound;
    state.saver.user_comp = vset_->inter_comp_.user_comparator();
    state.saver.user_key = key.user_key();
    state.saver.value = val;

    CallForEachOverlapSST(state.saver.user_key, state.ikey, &state, &State::Match);

    return state.found ? state.s : Status::NotFound(std::string_view());
}

bool Version::UpdateStats(const GetStats& stats) {
    SSTMetaData* sst = stats.seek_file;
    if (sst != nullptr) {
        sst->allowed_seeks--;
        if (sst->allowed_seeks <= 0 && sst_to_compact_ == nullptr) {
            sst_to_compact_ = sst;
            sst_to_compact_level_ = stats.seek_file_level;
            return true;
        }
    }
    return false;
}

bool Version::RecordReadSample(std::string_view internal_key) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(internal_key, &ikey)) {
        return false;
    }

    struct State {
        GetStats stats;
        int matches;

        static bool Match(void* arg, int level, SSTMetaData* sst) {
            State* state = reinterpret_cast<State*>(arg);
            state->matches++;
            if (state->matches == 1) {
                state->stats.seek_file = sst;
                state->stats.seek_file_level = level;
            }
            return state->matches < 2;
        }
    };

    State state;
    state.matches = 0;
    CallForEachOverlapSST(ikey.user_key, internal_key, &state, &State::Match);

    if (state.matches >= 2) {
        return UpdateStats(state.stats);
    }
    return false;
}

void Version::Ref() { refs_++; }

void Version::Unref() {
    assert(this != &vset_->dummy_versions_);
    assert(refs_ >= 1);
    refs_--;
    if (refs_ == 0) {
        delete this;
    }
}

bool Version::IsOverlapInLevel(int level, const std::string_view* smallest_user_key,
                               const std::string_view* largest_user_key) {
    return IsSomeFileOverlapsRange(vset_->inter_comp_, (level > 0), ssts_[level], smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemTableCompactionOutput(const std::string_view& smallest_user_key,
                                                  const std::string_view& largest_user_key) {
    int level = 0;

    // 如果 Level 0 没有重叠，尝试提升
    if (!IsOverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
        InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
        std::vector<SSTMetaData*> overlaps;
        while (level < gDBConfig->max_mem_compact_level) {
            // 如果下一层有重叠，停止提升
            if (IsOverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
                break;
            }

            // 检查 grandparent 层的重叠大小
            if (level + 2 < gDBConfig->num_levels) {
                // 找出某层与范围重叠的文件
                GetOverlapSSTs(level + 2, &start, &limit, &overlaps);
                const int64_t sum = TotalSSTSize(overlaps);
                if (sum > MaxGrandParentOverlapBytes()) {
                    // 重叠过大，停止提升
                    break;
                }
            }
            level++;
        }
    }
    return level;
}

void Version::GetOverlapSSTs(int level, const InternalKey* begin, const InternalKey* end,
                             std::vector<SSTMetaData*>* ssts) {
    assert(level >= 0);
    assert(level < gDBConfig->num_levels);
    ssts->clear();

    std::string_view user_begin, user_end;
    if (begin != nullptr) {
        user_begin = begin->user_key();
    }
    if (end != nullptr) {
        user_end = end->user_key();
    }

    const Comparator* user_comp = vset_->inter_comp_.user_comparator();
    for (size_t i = 0; i < ssts_[level].size();) {
        SSTMetaData* sst = ssts_[level][i++];
        const std::string_view sst_start = sst->smallest_key.user_key();
        const std::string_view sst_limit = sst->largest_key.user_key();
        if (begin != nullptr && user_comp->Compare(sst_limit, user_begin) < 0) {
            // 在该sst涉及范围之前
        } else if (end != nullptr && user_comp->Compare(sst_start, user_end) > 0) {
            // 在该sst涉及范围之后
        } else {
            // 范围有交叉
            ssts->push_back(sst);
            if (level == 0) {
                if (begin != nullptr && user_comp->Compare(sst_start, user_begin) < 0) {
                    user_begin = sst_start;
                    ssts->clear();
                    i = 0;
                } else if (end != nullptr && user_comp->Compare(sst_limit, user_end) > 0) {
                    user_end = sst_limit;
                    ssts->clear();
                    i = 0;
                }
            }
        }
    }
}

/**
 * @brief 版本构建器。 高效地将 VersionEdit 序列应用到 Version，避免创建中间版本。
 * 设计：
 *  - 维护每个 level 的 deleted_files 和 added_files
 *  - Apply() 累积所有编辑
 *  - SaveTo() 一次性合并到目标 Version
 */
class VersionSet::Builder {
   private:
    // 按 smallest key 排序文件
    struct OrderBySmallestKey {
        const InternalKeyComparator* internal_comparator;

        bool operator()(SSTMetaData* sst1, SSTMetaData* sst2) const {
            int r = internal_comparator->Compare(sst1->smallest_key, sst2->smallest_key);
            if (r != 0) {
                return (r < 0);
            } else {
                // 按文件编号打破平局
                return (sst1->sst_number < sst2->sst_number);
            }
        }
    };

    typedef std::set<SSTMetaData*, OrderBySmallestKey> SSTSet;
    struct LevelState {
        std::set<uint64_t> deleted_ssts;  // 已删除的文件编号
        SSTSet* added_ssts;               // 新增的文件
    };

    VersionSet* vset_;
    Version* base_;  // 基础版本
    LevelState levels_[7];

   public:
    // 构造函数，用 base 的文件初始化
    Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
        base_->Ref();
        OrderBySmallestKey comp;
        comp.internal_comparator = &vset_->inter_comp_;
        for (int level = 0; level < gDBConfig->num_levels; level++) {
            levels_[level].added_ssts = new SSTSet(comp);
        }
    }

    ~Builder() {
        for (int level = 0; level < gDBConfig->num_levels; level++) {
            const SSTSet* added = levels_[level].added_ssts;
            std::vector<SSTMetaData*> to_unref;
            to_unref.reserve(added->size());
            for (SSTSet::const_iterator it = added->begin(); it != added->end(); it++) {
                to_unref.push_back(*it);
            }
            delete added;
            // 释放添加但未使用的文件
            for (uint32_t i = 0; i < to_unref.size(); i++) {
                SSTMetaData* sst = to_unref[i];
                sst->refs--;
                if (sst->refs <= 0) {
                    delete sst;
                }
            }
        }
        base_->Unref();
    }

    /**
     * @brief 应用 VersionEdit
     */
    void Apply(const VersionEdit* edit) {
        // 更新 compaction 指针
        for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
            const int level = edit->compact_pointers_[i].first;
            vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode();
        }

        // 删除文件
        for (const auto& deleted_file_set_kvp : edit->deleted_ssts_) {
            const int level = deleted_file_set_kvp.first;
            const uint64_t number = deleted_file_set_kvp.second;
            levels_[level].deleted_ssts.insert(number);
        }

        // 添加新文件
        for (size_t i = 0; i < edit->new_ssts_.size(); i++) {
            const int level = edit->new_ssts_[i].first;
            SSTMetaData* sst = new SSTMetaData(edit->new_ssts_[i].second);
            sst->refs = 1;

            // 计算 allowed_seeks：基于文件大小
            // 假设：
            //   (1) 一次 Seek 耗时 10ms
            //   (2) 读/写 1MB 耗时 10ms (100MB/s)
            //   (3) 1MB compaction 产生 25MB IO:
            //         1MB 从本层读取
            //         10-12MB 从下一层读取
            //         10-12MB 写入下一层
            // 推论：25 次 Seek ≈ 1MB compaction 的代价
            //       1 次 Seek ≈ 40KB compaction 的代价
            // 保守设置：每 16KB 允许 1 次 Seek
            sst->allowed_seeks = static_cast<int>(sst->sst_size / 16384U);
            if (sst->allowed_seeks < 100) sst->allowed_seeks = 100;

            levels_[level].deleted_ssts.erase(sst->sst_number);
            levels_[level].added_ssts->insert(sst);
        }
    }

    /**
     * @brief 保存当前状态到 v
     */
    void SaveTo(Version* v) {
        OrderBySmallestKey comp;
        comp.internal_comparator = &vset_->inter_comp_;
        for (int level = 0; level < gDBConfig->num_levels; level++) {
            // 合并：base 文件 + added 文件 - deleted 文件
            const std::vector<SSTMetaData*>& base_ssts = base_->ssts_[level];
            std::vector<SSTMetaData*>::const_iterator base_iter = base_ssts.begin();
            std::vector<SSTMetaData*>::const_iterator base_end = base_ssts.end();
            const SSTSet* added_files = levels_[level].added_ssts;
            v->ssts_[level].reserve(base_ssts.size() + added_files->size());
            for (const auto& added_file : *added_files) {
                // Add all smaller files listed in base_
                for (std::vector<SSTMetaData*>::const_iterator bpos =
                         std::upper_bound(base_iter, base_end, added_file, comp);
                     base_iter != bpos; ++base_iter) {
                    MaybeAddFile(v, level, *base_iter);
                }

                MaybeAddFile(v, level, added_file);
            }

            // Add remaining base files
            for (; base_iter != base_end; ++base_iter) {
                MaybeAddFile(v, level, *base_iter);
            }
        }
    }

    /**
     * @brief 添加文件到 Version（如果未被删除）
     */
    void MaybeAddFile(Version* v, int level, SSTMetaData* sst) {
        if (levels_[level].deleted_ssts.count(sst->sst_number) > 0) {
            // File is deleted: do nothing
        } else {
            std::vector<SSTMetaData*>* ssts = &v->ssts_[level];
            if (level > 0 && !ssts->empty()) {
                // Must not overlap
                assert(vset_->inter_comp_.Compare((*ssts)[ssts->size() - 1]->largest_key, sst->smallest_key) < 0);
            }
            sst->refs++;
            ssts->push_back(sst);
        }
    }
};

VersionSet::VersionSet(const std::string& dbname, SSTCache* sst_cache, const InternalKeyComparator* comp)
    : dbname_(dbname),
      sst_cache_(sst_cache),
      inter_comp_(*comp),
      next_file_number_(2),
      manifest_number_(0),
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      manifest_(nullptr),
      manifest_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
    // 创建初始版本，但不通过 AppendVersion 设置 current_
    Version* initial = new Version(this);
    current_ = initial;
    current_->Ref();  // refs_ = 1

    // 加入双向链表
    current_->prev_ = &dummy_versions_;
    current_->next_ = &dummy_versions_;
    dummy_versions_.prev_ = current_;
    dummy_versions_.next_ = current_;
}

VersionSet::~VersionSet() {
    // 先解除 current_ 的引用（但不一定删除，因为可能还有其他引用）
    if (current_ != nullptr) {
        current_->Unref();
    }

    // 确保所有版本都已从链表中移除
    while (dummy_versions_.next_ != &dummy_versions_) {
        Version* v = dummy_versions_.next_;
        v->Unref();
    }

    delete manifest_log_;
    delete manifest_;
}

void VersionSet::AppendVersion(Version* v) {
    assert(v->refs_ == 0);
    assert(v != current_);
    if (current_ != nullptr) {
        current_->Unref();
    }
    current_ = v;
    v->Ref();

    // 加入双向链表
    v->prev_ = dummy_versions_.prev_;
    v->next_ = &dummy_versions_;
    v->prev_->next_ = v;
    v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::mutex* mtx) {
    // 补充 edit 的默认字段（wal_number, next_file_number 等）
    if (edit->has_wal_number_) {
        assert(edit->wal_number_ >= log_number_);
        assert(edit->wal_number_ < next_file_number_);
    } else {
        edit->SetWalNumber(log_number_);
    }

    if (!edit->has_prev_wal_number_) {
        edit->SetPrevWalNumber(prev_log_number_);
    }

    edit->SetNextSST(next_file_number_);
    edit->SetLastSequence(last_sequence_);

    // 构建新 Version
    Version* v = new Version(this);
    {
        Builder builder(this, current_);
        builder.Apply(edit);
        builder.SaveTo(v);
    }
    Finalize(v);  // 计算 compaction 分数

    // 如果需要，创建新的 MANIFEST 文件
    std::string new_manifest_filename;
    Status s;
    if (manifest_log_ == nullptr) {
        assert(manifest_ == nullptr);
        new_manifest_filename = ManifestFileName(dbname_, manifest_number_);
        s = NewWritableFile(new_manifest_filename, &manifest_);
        if (s.ok()) {
            manifest_log_ = new Writer(manifest_);
            s = WriteSnapshot(manifest_log_);  // 写入完整快照
        }
    }

    // 写入 MANIFEST 日志（释放锁以提高并发）
    {
        mtx->unlock();

        if (s.ok()) {
            std::string record;
            edit->EncodeTo(&record);
            s = manifest_log_->AddRecord(record);
            if (s.ok()) {
                s = manifest_->Sync();
            }
            if (!s.ok()) {
                InfoLog << "MANIFEST write: " << s.ToString() << std::endl;
            }
        }

        if (s.ok() && !new_manifest_filename.empty()) {
            s = SetCurrentFile(dbname_, manifest_number_);
        }

        mtx->lock();
    }

    // 安装新版本
    if (s.ok()) {
        AppendVersion(v);
        log_number_ = edit->wal_number_;
        prev_log_number_ = edit->prev_wal_number_;
    } else {
        delete v;
        if (!new_manifest_filename.empty()) {
            delete manifest_log_;
            delete manifest_;
            manifest_log_ = nullptr;
            manifest_ = nullptr;
            RemoveFile(new_manifest_filename);
        }
    }
    return s;
}

Status VersionSet::Recover(bool* save_manifest) {
    struct WalReporter : public Reader::Reporter {
        Status* status;
        void Corruption(size_t /*bytes*/, const Status& s) override {
            if (this->status->ok()) *this->status = s;
        }
    };

    // 读取 current 文件
    std::string current;
    Status s = ReadFileToString(CurrentFileName(dbname_), &current);
    if (!s.ok()) {
        return s;
    }
    // 检查读取的 current
    if (current.empty() || current[current.size() - 1] != '\n') {
        return Status::Corruption("CURRENT file does not end with newline");
    }
    current.resize(current.size() - 1);

    // 打开 MANIFEST 文件
    std::string manifest_name = dbname_ + "/" + current;
    SequentialFile* file;
    s = NewSequentialFile(manifest_name, &file);
    if (!s.ok()) {
        if (s.IsNotFound()) {
            return Status::Corruption("CURRENT points to a non-existent file", s.ToString());
        }
        return s;
    }

    // 重放所有 VersionSet
    bool have_log_number = false;
    bool have_prev_log_number = false;
    bool have_next_file = false;
    bool have_last_sequence = false;
    uint64_t next_file = 0;
    uint64_t last_sequence = 0;
    uint64_t log_number = 0;
    uint64_t prev_log_number = 0;
    Builder builder(this, current_);
    int read_records = 0;

    {
        WalReporter reporter;
        reporter.status = &s;
        Reader reader(file, &reporter, true, 0);
        std::string_view record;
        std::string scratch;
        while (reader.ReadRecord(&record, &scratch) && s.ok()) {
            read_records++;
            VersionEdit edit;
            s = edit.DecodeFrom(record);
            if (s.ok()) {
                if (edit.has_comparator_ && edit.comparator_ != inter_comp_.user_comparator()->Name()) {
                    s = Status::InvalidArgument(edit.comparator_ + " does not match existing comparator ",
                                                inter_comp_.user_comparator()->Name());
                }
            }

            if (s.ok()) {
                builder.Apply(&edit);
            }

            // 记录元信息
            if (edit.has_wal_number_) {
                log_number = edit.wal_number_;
                have_log_number = true;
            }

            if (edit.has_prev_wal_number_) {
                prev_log_number = edit.prev_wal_number_;
                have_prev_log_number = true;
            }

            if (edit.has_next_sst_number_) {
                next_file = edit.next_sst_number_;
                have_next_file = true;
            }

            if (edit.has_last_sequence_) {
                last_sequence = edit.last_sequence_;
                have_last_sequence = true;
            }
        }
    }

    delete file;
    file = nullptr;

    // 验证必要的元信息
    if (s.ok()) {
        if (!have_next_file) {
            s = Status::Corruption("no meta-nextfile entry in descriptor");
        } else if (!have_log_number) {
            s = Status::Corruption("no meta-lognumber entry in descriptor");
        } else if (!have_last_sequence) {
            s = Status::Corruption("no last-sequence-number entry in descriptor");
        }

        if (!have_prev_log_number) {
            prev_log_number = 0;
        }

        MarkFileNumberAsUsed(prev_log_number);
        MarkFileNumberAsUsed(log_number);
    }

    // 安装恢复的 Version
    if (s.ok()) {
        Version* v = new Version(this);
        builder.SaveTo(v);
        Finalize(v);
        AppendVersion(v);
        manifest_number_ = next_file;
        next_file_number_ = next_file + 1;
        last_sequence_ = last_sequence;
        log_number_ = log_number;
        prev_log_number_ = prev_log_number;

        // 检查是否可以重用 MANIFEST
        if (ReuseManifest(manifest_name, current)) {
            // 重用成功，不需要保存
        } else {
            *save_manifest = true;
        }
    } else {
        std::string error = s.ToString();
        InfoLog << std::format("Error recovering version set with {} records: {}", read_records, error);
    }

    return s;
}

bool VersionSet::ReuseManifest(const std::string& manifest_name, const std::string& manifest_base) {
    if (!gDBConfig->reuse_logs) {
        return false;
    }

    FileType manifest_type;
    uint64_t manifest_number;
    uint64_t manifest_size;
    if (!ParseFileName(manifest_base, &manifest_number, &manifest_type) || manifest_type != kManifestFile ||
        !GetFileSize(manifest_name, &manifest_size).ok() ||
        // Make new compacted MANIFEST if old one is too big
        manifest_size >= TargetFileSize()) {
        return false;
    }

    assert(manifest_ == nullptr);
    assert(manifest_log_ == nullptr);
    Status r = NewAppendableFile(manifest_name, &manifest_);
    if (!r.ok()) {
        InfoLog << "Reuse MANIFEST: " << r.ToString() << std::endl;
        assert(manifest_ == nullptr);
        return false;
    }

    InfoLog << "Reusing MANIFEST: " << manifest_name << std::endl;
    manifest_log_ = new Writer(manifest_, manifest_size);
    manifest_number_ = manifest_number;
    return true;
}

void VersionSet::MarkFileNumberAsUsed(uint64_t number) {
    if (next_file_number_ <= number) {
        next_file_number_ = number + 1;
    }
}

void VersionSet::Finalize(Version* v) {
    int best_level = -1;
    double best_score = -1;

    for (int level = 0; level < gDBConfig->num_levels - 1; level++) {
        double score;
        if (level == 0) {
            score = v->ssts_[level].size() / static_cast<double>(gDBConfig->l0_compaction_trigger);
        } else {
            const uint64_t level_bytes = TotalSSTSize(v->ssts_[level]);
            score = static_cast<double>(level_bytes) / MaxBytesForLevel(level);
        }

        if (score > best_score) {
            best_level = level;
            best_score = score;
        }
    }

    v->compaction_level_ = best_level;
    v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(Writer* log) {
    VersionEdit edit;
    edit.SetComparatorName(inter_comp_.user_comparator()->Name());

    for (int level = 0; level < gDBConfig->num_levels; level++) {
        if (!compact_pointer_[level].empty()) {
            InternalKey key;
            key.DecodeFrom(compact_pointer_[level]);
            edit.AppendCompactPointer(level, key);
        }
    }

    for (int level = 0; level < gDBConfig->num_levels; level++) {
        const std::vector<SSTMetaData*>& ssts = current_->ssts_[level];
        for (size_t i = 0; i < ssts.size(); i++) {
            const SSTMetaData* sst = ssts[i];
            edit.AddSST(level, sst->sst_number, sst->sst_size, sst->smallest_key, sst->largest_key);
        }
    }

    std::string record;
    edit.EncodeTo(&record);
    return log->AddRecord(record);
}

int VersionSet::SSTNumOfLevel(int level) const {
    assert(level >= 0);
    assert(level < gDBConfig->num_levels);
    return current_->ssts_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
    std::snprintf(scratch->buffer, sizeof(scratch->buffer), "files[ %d %d %d %d %d %d %d ]",
                  int(current_->ssts_[0].size()), int(current_->ssts_[1].size()), int(current_->ssts_[2].size()),
                  int(current_->ssts_[3].size()), int(current_->ssts_[4].size()), int(current_->ssts_[5].size()),
                  int(current_->ssts_[6].size()));
    return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
    uint64_t result = 0;
    for (int level = 0; level < gDBConfig->num_levels; level++) {
        const std::vector<SSTMetaData*>& ssts_of_level = v->ssts_[level];
        for (size_t i = 0; i < ssts_of_level.size(); i++) {
            if (inter_comp_.Compare(ssts_of_level[i]->largest_key, ikey) <= 0) {
                // 该 sst 都完全在 ikey 之前，直接加上偏移
                result += ssts_of_level[i]->sst_size;
            } else if (inter_comp_.Compare(ssts_of_level[i]->smallest_key, ikey) > 0) {
                // 该sst都完全在 ikey 之后，忽略
                if (level > 0) {
                    break;
                }
            } else {
                // ikey 落在该 sst 范围内
                Table* tableptr;
                // 计算 ikey 在该文件内的偏移
                Iterator* iter = sst_cache_->NewIterator(ReadOptions(), ssts_of_level[i]->sst_number,
                                                         ssts_of_level[i]->sst_size, &tableptr);
                if (tableptr != nullptr) {
                    result += tableptr->ApproximateOffsetOf(ikey.Encode());
                }
                delete iter;
            }
        }
    }
    return result;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
    // 遍历双向循环链表
    for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
        for (int level = 0; level < gDBConfig->num_levels; level++) {
            const std::vector<SSTMetaData*>& ssts = v->ssts_[level];
            for (size_t i = 0; i < ssts.size(); i++) {
                live->insert(ssts[i]->sst_number);
            }
        }
    }
}

int64_t VersionSet::SSTBytesOfLevel(int level) const {
    assert(level >= 0);
    assert(level < gDBConfig->num_levels);
    return TotalSSTSize(current_->ssts_[level]);
}

int64_t VersionSet::MaxNextLevelOverlapBytes() {
    int64_t result = 0;
    std::vector<SSTMetaData*> overlaps;
    for (int level = 1; level < gDBConfig->num_levels - 1; level++) {
        for (size_t i = 0; i < current_->ssts_[level].size(); i++) {
            // 逐层逐文件
            const SSTMetaData* sst = current_->ssts_[level][i];
            // 找出与该文件有重叠的下层文件(每次自动清空 overlaps)
            current_->GetOverlapSSTs(level + 1, &sst->smallest_key, &sst->largest_key, &overlaps);
            // 计算重叠文件总大小
            const int64_t sum = TotalSSTSize(overlaps);
            if (sum > result) {
                result = sum;
            }
        }
    }
    return result;
}

void VersionSet::GetRange(const std::vector<SSTMetaData*>& inputs, InternalKey* smallest_key,
                          InternalKey* largest_key) {
    assert(!inputs.empty());
    smallest_key->Clear();
    largest_key->Clear();
    for (size_t i = 0; i < inputs.size(); i++) {
        SSTMetaData* sst = inputs[i];
        if (i == 0) {
            // 相当于初始化
            *smallest_key = sst->smallest_key;
            *largest_key = sst->largest_key;
        } else {
            if (inter_comp_.Compare(sst->smallest_key, *smallest_key) < 0) {
                *smallest_key = sst->smallest_key;
            }
            if (inter_comp_.Compare(sst->largest_key, *largest_key) > 0) {
                *largest_key = sst->largest_key;
            }
        }
    }
}

void VersionSet::GetRange2(const std::vector<SSTMetaData*>& inputs1, const std::vector<SSTMetaData*>& inputs2,
                           InternalKey* smallest_key, InternalKey* largest_key) {
    // 将两个数组合二为一，直接复用上面的方法
    std::vector<SSTMetaData*> all = inputs1;
    all.insert(all.end(), inputs2.begin(), inputs2.end());
    GetRange(all, smallest_key, largest_key);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
    ReadOptions options;
    options.verify_checksums = gDBConfig->paranoid_checks;
    options.fill_cache = false;

    // c 内包含着两层 sst 文件
    // 如果是从 L0 合并到更高层，则 L0 的每个文件创建一个迭代器，另一层整体创建一个
    // 如果不包括 L0，则 c 的两层各自创建一个迭代器
    const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
    Iterator** list = new Iterator*[space];
    int num = 0;
    for (int which = 0; which < 2; which++) {
        if (!c->inputs_[which].empty()) {
            if (c->level() + which == 0) {
                const std::vector<SSTMetaData*>& ssts = c->inputs_[which];
                for (size_t i = 0; i < ssts.size(); i++) {
                    list[num++] = sst_cache_->NewIterator(options, ssts[i]->sst_number, ssts[i]->sst_size);
                }
            } else {
                list[num++] = NewTwoLevelIterator(new Version::LevelSSTNumIterator(inter_comp_, &c->inputs_[which]),
                                                  &GetSSTIterator, sst_cache_, options);
            }
        }
    }
    assert(num <= space);
    // 合并迭代器
    Iterator* result = NewMergeIterator(&inter_comp_, list, num);
    delete[] list;
    return result;
}

Compaction* VersionSet::PickCompaction() {
    Compaction* c;
    int level;

    // 基于大小的 compaction（某层文件太多）
    const bool size_compaction = (current_->compaction_score_ >= 1);
    // 基于 Seek 的 compaction（某文件读压力太大）
    const bool seek_compaction = (current_->sst_to_compact_ != nullptr);

    if (size_compaction) {
        level = current_->compaction_level_;
        assert(level >= 0);
        assert(level + 1 < gDBConfig->num_levels);
        c = new Compaction(level);

        // 选择 compact_pointer 之后的第一个文件
        for (size_t i = 0; i < current_->ssts_[level].size(); i++) {
            SSTMetaData* sst = current_->ssts_[level][i];
            if (compact_pointer_[level].empty() ||
                inter_comp_.Compare(sst->largest_key.Encode(), compact_pointer_[level]) > 0) {
                c->inputs_[0].push_back(sst);
                break;
            }
        }
        if (c->inputs_[0].empty()) {
            // 回绕到 key 空间起点
            c->inputs_[0].push_back(current_->ssts_[level][0]);
        }
    } else if (seek_compaction) {
        level = current_->sst_to_compact_level_;
        c = new Compaction(level);
        c->inputs_[0].push_back(current_->sst_to_compact_);
    } else {
        return nullptr;  // 不需要 compaction
    }

    c->input_version_ = current_;
    c->input_version_->Ref();

    // Level 0 文件可能重叠，需要选择所有重叠的文件
    if (level == 0) {
        InternalKey smallest_k, largest_k;
        GetRange(c->inputs_[0], &smallest_k, &largest_k);
        current_->GetOverlapSSTs(0, &smallest_k, &largest_k, &c->inputs_[0]);
        assert(!c->inputs_[0].empty());
    }

    // 设置 level+1 的输入文件
    SetupOtherInputs(c);
    return c;
}

// 找出 sst 数组中最大的 key，sst 数组不空就返回 true
bool FindLargestKey(const InternalKeyComparator& inter_comp, const std::vector<SSTMetaData*>& ssts,
                    InternalKey* largest_key) {
    if (ssts.empty()) {
        return false;
    }
    *largest_key = ssts[0]->largest_key;
    for (size_t i = 1; i < ssts.size(); i++) {
        SSTMetaData* sst = ssts[i];
        if (inter_comp.Compare(sst->largest_key, *largest_key) > 0) {
            *largest_key = sst->largest_key;
        }
    }
    return true;
}

/**
 * @brief 在指定层级的文件列表中，查找最小的（起始 key 最靠前）且满足起始 key > largest_key 同时 user_key
 * 与之相等的边界文件。
 * 这种逻辑通常用于处理重叠的Key范围。当一个文件的结束Key与另一个文件的起始Key拥有相同的UserKey但不同的 Internal
 * Key（例如不同的序列号或类型）时，数据库需要找到这个“下一个”文件，以确保Compaction 能够正确处理所有版本的同一个User
 * Key，防止数据丢失或版本回退。
 */
SSTMetaData* FindSmallestBoundarySST(const InternalKeyComparator& inter_comp,
                                     const std::vector<SSTMetaData*>& level_ssts, const InternalKey& largest_key) {
    const Comparator* user_comp = inter_comp.user_comparator();
    SSTMetaData* smallest_boundary_sst = nullptr;
    for (size_t i = 0; i < level_ssts.size(); i++) {
        SSTMetaData* sst = level_ssts[i];
        if (inter_comp.Compare(sst->smallest_key, largest_key) > 0 &&
            user_comp->Compare(sst->smallest_key.user_key(), largest_key.user_key()) == 0) {
            // 意味着虽然 InternalKey 变大了（可能是因为序列号变大），但它们指向的用户数据键是同一个
            if (smallest_boundary_sst == nullptr ||
                inter_comp.Compare(sst->smallest_key, smallest_boundary_sst->smallest_key) < 0) {
                // 满足必要条件且更贴近边界的 sst
                smallest_boundary_sst = sst;
            }
        }
    }
    return smallest_boundary_sst;
}

/**
 * @brief 边界文件扩展，在compaction时强制把首尾相连的文件一起打包处理。
 * 确保同一键值范围的数据在层级变动时保持原子性和一致性，从而保证读取的正确性。
 */
void AddBoundaryInputs(const InternalKeyComparator& inter_comp, const std::vector<SSTMetaData*>& level_ssts,
                       std::vector<SSTMetaData*>* compaction_ssts) {
    InternalKey largest_key;

    // 从当前准备 compact 的文件列表中找出最大键值 (largest_key) 的文件，相当于确定当前这一批数据的右边界
    if (!FindLargestKey(inter_comp, *compaction_ssts, &largest_key)) {
        return;
    }

    bool continue_search = true;
    while (continue_search) {
        // 拿着这个右边界去待选的文件堆（level_ssts）里找，看有没有哪个文件的最小键值（smallest_key）刚好跟它相等
        SSTMetaData* smallest_boundary_sst = FindSmallestBoundarySST(inter_comp, level_ssts, largest_key);

        // 如果有，说明这两个文件在数据范围上是首尾相接的，这个新找到的文件就是边界文件
        if (smallest_boundary_sst != NULL) {
            // 找到这种邻居文件后把它拉近压缩列表
            compaction_ssts->push_back(smallest_boundary_sst);
            // 以这个新加入文件的最大键值为新的基准，重复上述搜索过程，直到找不到下一个首尾相接的文件为止
            largest_key = smallest_boundary_sst->largest_key;
        } else {
            continue_search = false;
        }
    }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
    const int level = c->level();
    InternalKey smallest_k, largest_k;

    // 添加边界文件，避免数据不一致
    AddBoundaryInputs(inter_comp_, current_->ssts_[level], &c->inputs_[0]);
    GetRange(c->inputs_[0], &smallest_k, &largest_k);

    // 获取 level+1 层与 inputs[0] 范围有重叠的文件
    current_->GetOverlapSSTs(level + 1, &smallest_k, &largest_k, &c->inputs_[1]);
    AddBoundaryInputs(inter_comp_, current_->ssts_[level + 1], &c->inputs_[1]);

    InternalKey all_start, all_limit;  // 全部两层文件的最大、最小 key
    GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

    // 尝试在不增加 inputs[1] 的前提下回过头用新范围扩展 inputs[0]
    if (!c->inputs_[1].empty()) {
        std::vector<SSTMetaData*> expanded0;  // 暂存可能扩展过的 inputs[0]
        current_->GetOverlapSSTs(level, &all_start, &all_limit, &expanded0);
        AddBoundaryInputs(inter_comp_, current_->ssts_[level], &expanded0);

        const int64_t inputs0_size = TotalSSTSize(c->inputs_[0]);
        const int64_t inputs1_size = TotalSSTSize(c->inputs_[1]);
        const int64_t expanded0_size = TotalSSTSize(expanded0);

        // inputs[0] 得到扩展，接下来要看是否会影响 inputs[1]
        if (expanded0.size() > c->inputs_[0].size() &&
            inputs1_size + expanded0_size < ExpandedCompactionByteSizeLimit()) {
            InternalKey new_start, new_limit;
            GetRange(expanded0, &new_start, &new_limit);
            std::vector<SSTMetaData*> expanded1;
            current_->GetOverlapSSTs(level + 1, &new_start, &new_limit, &expanded1);
            AddBoundaryInputs(inter_comp_, current_->ssts_[level + 1], &expanded1);

            if (expanded1.size() == c->inputs_[1].size()) {
                // inputs[1] 数量不变，扩展成功
                InfoLog << std::format("Expanding@{} {}+{} ({}+{} bytes) to {}+{} ({}+{} bytes)", level,
                                       c->inputs_[0].size(), c->inputs_[1].size(), inputs0_size, inputs1_size,
                                       expanded0.size(), expanded1.size(), expanded0_size, inputs1_size);
                smallest_k = new_start;
                largest_k = new_limit;
                c->inputs_[0] = expanded0;
                c->inputs_[1] = expanded1;
                GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
            }
        }
    }

    // 设置 grandparent 文件（level + 2）
    if (level + 2 < gDBConfig->num_levels) {
        current_->GetOverlapSSTs(level + 2, &all_start, &all_limit, &c->grandparents_);
    }

    // 更新 compact_pointer
    compact_pointer_[level] = largest_k.Encode();
    c->edit_.AppendCompactPointer(level, largest_k);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end) {
    std::vector<SSTMetaData*> inputs;
    // 在指定范围内初步选择需要 compact 的文件
    current_->GetOverlapSSTs(level, begin, end, &inputs);
    if (inputs.empty()) {
        return nullptr;
    }

    if (level > 0) {
        const uint64_t limit = MaxSSTSizeForLevel(level);  // 0 层之上，各层最大文件大小
        uint64_t total = 0;

        // 根据所选文件大小，灵活截断最终被选中的文件
        for (size_t i = 0; i < inputs.size(); i++) {
            uint64_t s = inputs[i]->sst_size;
            total += s;
            if (total >= limit) {
                inputs.resize(i + 1);
                break;
            }
        }
    }

    Compaction* c = new Compaction(level);
    c->input_version_ = current_;
    c->input_version_->Ref();
    c->inputs_[0] = inputs;
    SetupOtherInputs(c);
    return c;
}

Compaction::Compaction(int level)
    : level_(level),
      max_output_file_size_(MaxBytesForLevel(level)),
      input_version_(nullptr),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
    for (int i = 0; i < gDBConfig->num_levels; i++) {
        level_ptrs_[i] = 0;
    }
}

Compaction::~Compaction() {
    if (input_version_ != nullptr) {
        input_version_->Unref();
    }
}

bool Compaction::IsJustMove() const {
    return (input_files_num(0) == 1 && input_files_num(1) == 0 &&
            TotalSSTSize(grandparents_) <= MaxGrandParentOverlapBytes());
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
    for (int which = 0; which < 2; which++) {
        for (size_t i = 0; i < inputs_[which].size(); i++) {
            edit->RemoveSST(level_ + which, inputs_[which][i]->sst_number);
        }
    }
}

bool Compaction::IsBaseLevelForKey(const std::string_view& user_key) {
    const Comparator* user_comp = input_version_->vset_->inter_comp_.user_comparator();

    // 线性搜索 level+2 及更高层
    for (int lev = level_ + 2; lev < gDBConfig->num_levels; lev++) {
        const std::vector<SSTMetaData*>& ssts = input_version_->ssts_[lev];

        // 使用 level_ptrs_ 保持搜索位置（避免重复搜索）
        while (level_ptrs_[lev] < ssts.size()) {
            SSTMetaData* sst = ssts[level_ptrs_[lev]];
            if (user_comp->Compare(user_key, sst->largest_key.user_key()) <= 0) {
                // 已到达足够远的位置
                if (user_comp->Compare(user_key, sst->smallest_key.user_key()) >= 0) {
                    // key 落在此文件范围内，不是 base level
                    return false;
                }
                break;  // 此层没有更多相关文件
            }
            level_ptrs_[lev]++;
        }
    }
    return true;
}

bool Compaction::ShouldStopBefore(const std::string_view& internal_key) {
    const VersionSet* vset = input_version_->vset_;
    const InternalKeyComparator* inter_comp = &vset->inter_comp_;

    // 扫描找到第一个包含 key 的 grandparent 文件
    while (grandparent_index_ < grandparents_.size() &&
           inter_comp->Compare(internal_key, grandparents_[grandparent_index_]->largest_key.Encode()) > 0) {
        if (seen_key_) {
            // 累加重叠字节
            overlapped_bytes_ += grandparents_[grandparent_index_]->sst_size;
        }
        grandparent_index_++;
    }
    seen_key_ = true;

    // 检查重叠是否超过阈值
    if (overlapped_bytes_ > MaxGrandParentOverlapBytes()) {
        // 重叠过大，开始新文件
        overlapped_bytes_ = 0;
        return true;
    } else {
        return false;
    }
}

void Compaction::ReleaseInputs() {
    if (input_version_ != nullptr) {
        input_version_->Unref();
        input_version_ = nullptr;
    }
}

}  // namespace delta