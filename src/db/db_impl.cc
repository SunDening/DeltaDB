#include <unistd.h>
#include <mutex>

#include <deltadb/db/db.h>
#include <deltadb/db/db_impl.h>
#include <deltadb/db/db_iter.h>
#include <deltadb/db/filename.h>
#include <deltadb/db/memtable.h>
#include <deltadb/db/version_set.h>
#include <deltadb/db/write_batch_internal.h>
#include <deltadb/table/iter_merger.h>
#include <deltadb/table/sst_builder.h>
#include <deltadb/table/sst_cache.h>
#include <deltadb/table/table.h>
#include <deltadb/utils/coding.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/wal/wal_reader.h>
#include <deltadb/wal/wal_writer.h>

namespace delta {
delta::Config::ptr gDBConfig;
delta::Logger::ptr gDBLogger;

// ===========================================

// 为非 SST 表文件预留的文件描述符的数量
// 包括：日志文件、当前文件、锁文件、MANIFEST 文件等
const int kNonSSTCacheFilesNum = 10;

/**
 * @brief 写请求信息
 * 用于追踪每个等待的写请求，leveldb使用写队列来批量处理写操作以提高性能。
 * 写队列允许多个写请求合并成一个批量写入，减少 I/O 次数。
 */
struct DBImpl::WriteInfo {
    Status status;      // 写操作的状态
    WriteBatch* batch;  // 写入的数据批次
    bool sync;          // 是否需要同步到磁盘
    bool done;          // 写操作是否完成
    CondVar cv;         // 条件变量，用于写请求者等待完成

    explicit WriteInfo(std::mutex* mtx) : batch(nullptr), sync(false), done(false), cv(mtx) {}
};

/**
 * @brief 压缩操作状态结构体。记录一次 compaction 操作的中间状态，包括输入输出文件信息。
 */
struct DBImpl::CompactionState {
    // 输出文件信息
    struct Output {
        uint64_t sst_number;                // SST 文件编号
        uint64_t sst_size;                  // 文件大小
        InternalKey smallest_k, largest_k;  // 文件中的最小和最大内部键
    };

    Compaction* const compaction;      // 指向当前的压缩任务
    SequenceNumber smallest_snapshot;  // 需要服务的最小序列号
    std::vector<Output> outputs;       // 压缩产生的输出文件列表

    WritableFile* outfile;  // 输出文件句柄
    SSTBuilder* builder;    // SST 表构建器

    uint64_t total_bytes;  // 输出文件的总字节数

    explicit CompactionState(Compaction* c)
        : compaction(c), smallest_snapshot(0), outfile(nullptr), builder(nullptr), total_bytes(0) {}

    Output* current_output() { return &outputs[outputs.size() - 1]; }
};

/**
 * @brief 将用户提供的选项限制在合理范围内
 */
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
    if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
    if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}

/**
 * @brief 清理和验证配置，设置合适的默认值
 */
void SanitizeConfig(const std::string& /*dbname*/, const InternalKeyComparator* inter_comp,
                    const InternalFilterPolicy* ipolicy) {
    gDBConfig->comparator = inter_comp;
    gDBConfig->filter_policy = ipolicy;

    // 限制选项在合理范围
    ClipToRange(&gDBConfig->max_open_files, 64 + kNonSSTCacheFilesNum, 50000);
    ClipToRange(&gDBConfig->write_buffer_size, 64 << 10, 1 << 30);
    ClipToRange(&gDBConfig->max_file_size, 1 << 20, 1 << 30);
    ClipToRange(&gDBConfig->block_size, 1 << 10, 4 << 20);

    // 日志记录有单独的，不需要集成在 gDBConfig

    // 如果没有块缓存，创建一个 8MB 的 LRU 缓存
    if (gDBConfig->block_cache == nullptr) {
        gDBConfig->block_cache = NewLRUCache(8 << 20);
    }
}

/**
 * @brief 计算表缓存大小（从最大打开文件数中预留 10 个给其他文件）
 */
static int SSTCacheSize() { return gDBConfig->max_open_files - kNonSSTCacheFilesNum; }

DBImpl::DBImpl(const std::string& dbname)
    : internal_comparator_(gDBConfig->comparator),
      internal_filter_policy_(gDBConfig->filter_policy),
      dbname_(dbname),
      sst_cache_(new SSTCache(dbname_, SSTCacheSize())),
      shutting_down_(false),
      background_work_finished_signal_(&mtx_),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      wal_file_(nullptr),
      wal_file_number_(0),
      wal_writer_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, sst_cache_, &internal_comparator_)) {}

DBImpl::~DBImpl() {
    // 等待后台工作完成
    mtx_.lock();
    shutting_down_.store(true, std::memory_order_release);
    while (background_compaction_scheduled_) {
        background_work_finished_signal_.Wait();
    }
    mtx_.unlock();

    // 释放所有资源
    delete versions_;
    if (mem_ != nullptr) mem_->Unref();
    if (imm_ != nullptr) imm_->Unref();
    delete tmp_batch_;
    delete wal_writer_;
    delete wal_file_;
    delete sst_cache_;
}

Status DBImpl::NewDB() {
    VersionEdit new_db;
    new_db.SetComparatorName(user_comparator()->Name());
    new_db.SetWalNumber(0);
    new_db.SetNextSST(2);  // 下一个文件编号从 2 开始（1 已用于 MANIFEST）
    new_db.SetLastSequence(0);

    // 创建 MANIFEST 文件
    const std::string manifest = ManifestFileName(dbname_, 1);
    WritableFile* file;
    Status s = NewWritableFile(manifest, &file);
    if (!s.ok()) {
        return s;
    }
    {
        Writer wal(file);
        std::string record;
        new_db.EncodeTo(&record);
        s = wal.AddRecord(record);
        if (s.ok()) {
            s = file->Sync();
        }
        if (s.ok()) {
            s = file->Close();
        }
    }
    delete file;
    if (s.ok()) {
        // 创建 CURRENT 文件指向新的 MANIFEST
        s = SetCurrentFile(dbname_, 1);
    } else {
        RemoveFile(manifest);
    }
    return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
    if (s->ok() || gDBConfig->paranoid_checks) {
        // 不需要更改
    } else {
        // 忽略错误，记录日志
        InfoLog << "Ignoring error" << s->ToString();
        *s = Status::OK();
    }
}

void DBImpl::RemoveObsoleteFiles() {
    if (!bg_error_.ok()) {
        // 后台出错后，不知道新版本是否已提交，因此不能安全地垃圾回收
        return;
    }

    // 收集所有存活的文件集合
    std::set<uint64_t> live = pending_outputs_;
    versions_->AddLiveFiles(&live);

    std::vector<std::string> filenames;
    GetChildren(dbname_, &filenames);  // 获取所有子文件名（不含路径）
    uint64_t number;
    FileType type;
    std::vector<std::string> files_to_delete;
    // 遍历所有子文件名
    for (std::string& filename : filenames) {
        // 解析子文件名 编号，类型
        if (ParseFileName(filename, &number, &type)) {
            bool keep = true;
            switch (type) {
                case kWalFile:
                    keep = ((number >= versions_->LogNumber()) || (number == versions_->PrevWalNumber()));
                    break;
                case kManifestFile:
                    keep = (number >= versions_->ManifestFileNumber());
                    break;
                case kSSTFile:
                    keep = (live.find(number) != live.end());
                    break;
                case kTempFile:
                    keep = (live.find(number) != live.end());
                    break;
                case kCurrentFile:
                case kDBLockFile:
                case kInfoLogFile:
                    keep = true;
                    break;
            }

            if (!keep) {
                files_to_delete.push_back(std::move(filename));
                if (type == kSSTFile) {
                    sst_cache_->Evict(number);  // 从缓存中驱逐
                }
                InfoLog << std::format("Delete type={}, #{}", static_cast<int>(type),
                                       static_cast<unsigned long long>(number));
            }
        }
    }

    // 删除文件时释放锁，允许其他线程继续工作
    // 被删除的文件有唯一名称，不会与新创建的文件冲突
    mtx_.unlock();
    for (const std::string& filename : files_to_delete) {
        RemoveFile(dbname_ + "/" + filename);
    }
    mtx_.lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
    CreateDir(dbname_);

    Status s;
    // 如果数据库不存在，根据选项创建或返回错误
    if (!FileExists(CurrentFileName(dbname_))) {
        // 不存在
        if (gDBConfig->create_if_missing) {
            InfoLog << "Creating DB " << dbname_ << " since it was missing.";
            s = NewDB();
            if (!s.ok()) {
                return s;
            }
        } else {
            return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
        }
    } else {
        // 存在
        if (gDBConfig->error_if_exists) {
            return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
        }
    }

    // 从 MANIFEST 恢复版本信息
    s = versions_->Recover(save_manifest);
    if (!s.ok()) {
        return s;
    }
    SequenceNumber max_sequence(0);

    // 恢复比 MANIFEST 中记录的更新的日志文件
    // （前一次运行可能分配了日志编号但未注册到 MANIFEST）
    const uint64_t min_wal = versions_->LogNumber();       // 最小日志号
    const uint64_t prev_wal = versions_->PrevWalNumber();  // 上一个日志号
    std::vector<std::string> filenames;
    s = GetChildren(dbname_, &filenames);  // 获取数据库目录下所有的文件名
    if (!s.ok()) {
        return s;
    }
    std::set<uint64_t> expected;
    // 将当前版本认为“活跃”或“必须”的所有文件编号加入到一个集合 expected 中
    versions_->AddLiveFiles(&expected);
    uint64_t number;
    FileType type;
    std::vector<uint64_t> wals;
    for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type)) {
            expected.erase(number);
            if (type == kWalFile && ((number >= min_wal) || (number == prev_wal))) {
                wals.push_back(number);  // 加入待处理的 wals 列表
            }
        }
    }
    // expected 不为空，说明有文件只存在于版本记录中，却丢失于磁盘上
    if (!expected.empty()) {
        std::string msg = std::format("{} missing files; e.g.", static_cast<int>(expected.size()));
        // 数据库停止恢复，防止数据不一致
        return Status::Corruption(msg, SSTFileName(dbname_, *(expected.begin())));
    }

    // 按日志生成顺序恢复
    std::sort(wals.begin(), wals.end());
    for (size_t i = 0; i < wals.size(); i++) {
        s = RecoverWalFile(wals[i], (i == wals.size() - 1), save_manifest, edit, &max_sequence);
        if (!s.ok()) {
            return s;
        }

        // 前一次可能分配了日志号但未写入 MANIFEST 记录，所以手动更新 VersionSet 中的文件编号计数器
        versions_->MarkFileNumberAsUsed(wals[i]);
    }

    if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
    }
    return Status::OK();
}

Status DBImpl::RecoverWalFile(uint64_t wal_number, bool last_log, bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
    // 日志损坏报告器
    struct WalReporter : public Reader::Reporter {
        const char* fname;
        Status* status;
        void Corruption(size_t bytes, const Status& s) override {
            InfoLog << std::format("{}{}: dropping {} bytes; {}", (this->status == nullptr ? "(ignoring error) " : ""),
                                   fname, static_cast<int>(bytes), s.ToString());
            if (this->status != nullptr && this->status->ok()) {
                *this->status = s;
            }
        }
    };

    // 打开预写日志文件
    std::string fname = WalFileName(dbname_, wal_number);
    SequentialFile* file;
    Status status = NewSequentialFile(fname, &file);
    if (!status.ok()) {
        MaybeIgnoreError(&status);
        return status;
    }

    // 创建日志读取器
    WalReporter reporter;
    reporter.fname = fname.c_str();
    reporter.status = (gDBConfig->paranoid_checks ? &status : nullptr);
    // 即使 paranoid_checks==false 也进行校验和检查
    // 这样损坏会导致整个提交被跳过，而不是传播错误信息
    Reader reader(file, &reporter, true, 0);
    InfoLog << std::format("Recovering wal #{}", (unsigned long long)wal_number);

    // 读取所有记录并添加到 MemTable （回放操作）
    std::string scratch;
    std::string_view record;
    WriteBatch batch;
    int compactions = 0;
    MemTable* mem = nullptr;
    while (reader.ReadRecord(&record, &scratch) && status.ok()) {
        if (record.size() < 12) {
            reporter.Corruption(record.size(), Status::Corruption("wal record too small"));
            continue;
        }
        WriteBatchInternal::SetContents(&batch, record);

        if (mem == nullptr) {
            mem = new MemTable(internal_comparator_);
            mem->Ref();
        }
        status = WriteBatchInternal::InsertInto(&batch, mem);
        MaybeIgnoreError(&status);
        if (!status.ok()) {
            break;
        }
        const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
        if (last_seq > *max_sequence) {
            *max_sequence = last_seq;
        }

        if (mem->ApproximateMemoryUsage() > gDBConfig->write_buffer_size) {
            compactions++;
            *save_manifest = true;
            status = WriteToLevel0(mem, edit, nullptr);
            mem->Unref();
            mem = nullptr;
            if (!status.ok()) {
                break;
            }
        }
    }

    delete file;

    // 查看是否应该继续重用最后的日志文件
    if (status.ok() && gDBConfig->reuse_logs && last_log && compactions == 0) {
        assert(wal_file_ == nullptr);
        assert(wal_writer_ == nullptr);
        assert(mem_ == nullptr);
        uint64_t lfile_size;
        if (GetFileSize(fname, &lfile_size).ok() && NewAppendableFile(fname, &wal_file_).ok()) {
            InfoLog << "Reusing old wal: " << fname;
            wal_writer_ = new Writer(wal_file_, lfile_size);
            wal_file_number_ = wal_number;
            if (mem != nullptr) {
                mem_ = mem;
                mem = nullptr;
            } else {
                mem_ = new MemTable(internal_comparator_);
                mem_->Ref();
            }
        }
    }

    if (mem != nullptr) {
        // MemTable 未被重用。将其压缩为 SST 文件
        if (status.ok()) {
            *save_manifest = true;
            status = WriteToLevel0(mem, edit, nullptr);
        }
        mem->Unref();
    }

    return status;
}

Status DBImpl::WriteToLevel0(MemTable* mem, VersionEdit* edit, Version* base) {
    const uint64_t start_micros = NowMicros();
    SSTMetaData meta;
    meta.sst_number = versions_->NewFileNumber();
    pending_outputs_.insert(meta.sst_number);
    Iterator* iter = mem->NewIterator();
    InfoLog << std::format("Level-0 sst #{}: started", (unsigned long long)meta.sst_number);

    Status s;
    {
        mtx_.unlock();
        s = BuildSST(dbname_, sst_cache_, iter, &meta);
        mtx_.lock();
    }

    InfoLog << std::format("Level-0 sst #{}: {} bytes {}", (unsigned long long)meta.sst_number,
                           (unsigned long long)meta.sst_size, s.ToString());
    delete iter;
    pending_outputs_.erase(meta.sst_number);

    // 如果文件大小为0，文件已被删除，不应添加到 MANIFEST
    int level = 0;
    if (s.ok() && meta.sst_size > 0) {
        const std::string_view min_user_key = meta.smallest_key.user_key();
        const std::string_view max_user_key = meta.largest_key.user_key();
        if (base != nullptr) {
            // 根据键范围选择最优的 level（可能直接放入更高层级）
            level = base->PickLevelForMemTableCompactionOutput(min_user_key, max_user_key);
        }
        edit->AddSST(level, meta.sst_number, meta.sst_size, meta.smallest_key, meta.largest_key);
    }

    // 记录压缩统计信息
    CompactionStats stats;
    stats.micros = NowMicros() - start_micros;
    stats.bytes_written = meta.sst_size;
    stats_[level].Add(stats);
    return s;
}

void DBImpl::CompactMemTable() {
    assert(imm_ != nullptr);

    // 将 MemTable 内容保存为新的 SST 文件
    VersionEdit edit;
    Version* base = versions_->current();
    base->Ref();
    Status s = WriteToLevel0(imm_, &edit, base);
    base->Unref();

    if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
        s = Status::IOError("Deleting DB during memtable compaction");
    }

    // 用生成的 SST 文件替换不可变 MemTable
    if (s.ok()) {
        edit.SetPrevWalNumber(0);
        edit.SetWalNumber(wal_file_number_);
        s = versions_->LogAndApply(&edit, &mtx_);
    }

    if (s.ok()) {
        // 提交新状态
        imm_->Unref();
        imm_ = nullptr;
        has_imm_.store(false, std::memory_order_release);
        RemoveObsoleteFiles();
    } else {
        RecordBackgroundError(s);
    }
}

void DBImpl::CompactRange(const std::string_view* begin, const std::string_view* end) {
    int max_level_with_files = 1;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        Version* base = versions_->current();
        for (int level = 1; level < gDBConfig->num_levels; level++) {
            if (base->IsOverlapInLevel(level, begin, end)) {
                // 在该层有范围重叠
                max_level_with_files = level;
            }
        }
    }
    // 先刷新 MemTable （强制刷盘）
    TEST_CompactMemTable();

    // 重新计算 max_level_with_files，因为 TEST_CompactMemTable 可能在更高层级创建了 SST 文件
    {
        std::lock_guard<std::mutex> lock(mtx_);
        Version* base = versions_->current();
        for (int level = 1; level < gDBConfig->num_levels; level++) {
            if (base->IsOverlapInLevel(level, begin, end)) {
                max_level_with_files = level;
            }
        }
    }

    for (int level = 0; level < max_level_with_files; level++) {
        // 逐层compact与指定范围有重叠的sst
        TEST_CompactRange(level, begin, end);
    }
}

void DBImpl::TEST_CompactRange(int level, const std::string_view* begin, const std::string_view* end) {
    assert(level >= 0);
    assert(level + 1 < gDBConfig->num_levels);

    InternalKey begin_storage, end_storage;

    ManualCompaction manual;
    manual.level = level;
    manual.done = false;
    if (begin == nullptr) {
        manual.begin = nullptr;
    } else {
        begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
        manual.begin = &begin_storage;
    }
    if (end == nullptr) {
        manual.end = nullptr;
    } else {
        end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
        manual.end = &end_storage;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.ok()) {
        if (manual_compaction_ == nullptr) {
            // manual_compaction_ 空闲
            manual_compaction_ = &manual;
            MaybeScheduleCompaction();
        } else {
            // manual_compaction_ 不空闲，后台正在运行本次压缩或另一个压缩
            background_work_finished_signal_.Wait();
        }
    }

    // 在 background_work_finished_signal_ 因错误而发出信号的情况下完成当前的后台压缩
    while (background_compaction_scheduled_) {
        background_work_finished_signal_.Wait();
    }
    if (manual_compaction_ == &manual) {
        // 取消手动压缩
        manual_compaction_ = nullptr;
    }
}

Status DBImpl::TEST_CompactMemTable() {
    // nullptr batch 表示只等待之前的写入完成
    Status s = Write(WriteOptions(), nullptr);
    if (s.ok()) {
        std::lock_guard<std::mutex> lock(mtx_);
        // 等待压缩完成
        while (imm_ != nullptr && bg_error_.ok() && !shutting_down_.load(std::memory_order_acquire)) {
            background_work_finished_signal_.Wait();
        }
        if (imm_ != nullptr) {
            s = bg_error_;
        }
    }
    return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
    if (bg_error_.ok()) {
        // 记录后台错误
        bg_error_ = s;
        // 唤醒所有等待者
        background_work_finished_signal_.SignalAll();
    }
}

void DBImpl::MaybeScheduleCompaction() {
    if (background_compaction_scheduled_) {
        // 已经调度过了
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // 数据库正在删除，不再进行后台压缩
    } else if (!bg_error_.ok()) {
        // 发生后台错误，不再更改
    } else if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->IsNeedCompaction()) {
        // 不需要压缩
    } else {
        background_compaction_scheduled_ = true;
        thread_pool.Schedule(&DBImpl::BGWork, this);
    }
}

void DBImpl::BGWork(void* db) { reinterpret_cast<DBImpl*>(db)->BackgroundCall(); }

void DBImpl::BackgroundCall() {
    std::lock_guard<std::mutex> lock(mtx_);
    assert(background_compaction_scheduled_);
    if (shutting_down_.load(std::memory_order_acquire)) {
        // 关闭时不再进行后台工作
    } else if (!bg_error_.ok()) {
        // 后台错误后不再进行后台工作
    } else {
        BackgroundCompaction();  // 执行实际的压缩
    }

    background_compaction_scheduled_ = false;

    // 前一次压缩可能在某一层产生了太多文件
    // 因此如果需要，重新调度一次压缩
    MaybeScheduleCompaction();
    background_work_finished_signal_.SignalAll();  // 唤醒所有等待者
}

void DBImpl::BackgroundCompaction() {
    if (imm_ != nullptr) {
        CompactMemTable();
        return;
    }

    Compaction* c;
    bool is_manual = (manual_compaction_ != nullptr);
    InternalKey manual_end;
    if (is_manual) {
        ManualCompaction* m = manual_compaction_;
        // 在指定 level 的 [begin, end] 范围内选择需要进行压缩的文件并创建 compaction 对象
        c = versions_->CompactRange(m->level, m->begin, m->end);
        m->done = (c == nullptr);  // 如果该 level 没有文件与范围重叠时为 true
        if (c != nullptr) {
            manual_end = c->get_input_file(0, c->input_files_num(0) - 1)->largest_key;
        }
        InfoLog << std::format("Manual compaction at level-{} from {} .. {}; will stop at {}", m->level,
                               (m->begin ? m->begin->DebugString() : "(begin)"),
                               (m->end ? m->end->DebugString() : "(end)"),
                               (m->done ? "(end)" : manual_end.DebugString()));
    } else {
        c = versions_->PickCompaction();  // 自动选择压缩
    }

    Status status;
    if (c == nullptr) {
        // Nothing to do
    } else if (!is_manual && c->IsJustMove()) {
        // 无需合并，只需移动文件到其它层次
        assert(c->input_files_num(0) == 1);
        SSTMetaData* sst = c->get_input_file(0, 0);
        c->edit()->RemoveSST(c->level(), sst->sst_number);
        c->edit()->AddSST(c->level() + 1, sst->sst_number, sst->sst_size, sst->smallest_key, sst->largest_key);
        status = versions_->LogAndApply(c->edit(), &mtx_);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }
        VersionSet::LevelSummaryStorage tmp;
        InfoLog << std::format(
            "Moved #{} to level-{} {} bytes {}: {}", static_cast<unsigned long long>(sst->sst_number), c->level() + 1,
            static_cast<unsigned long long>(sst->sst_size), status.ToString(), versions_->LevelSummary(&tmp));
    } else {
        // 正常压缩：合并多个文件
        CompactionState* compact = new CompactionState(c);
        status = DoCompactionWork(compact);
        if (!status.ok()) {
            RecordBackgroundError(status);
        }
        CleanupCompaction(compact);
        c->ReleaseInputs();
        RemoveObsoleteFiles();
    }
    delete c;

    if (status.ok()) {
        // 完成
    } else if (shutting_down_.load(std::memory_order_acquire)) {
        // 关闭期间忽略压缩错误
    } else {
        // 没有成功，数据库也不是关闭状态，则发生错误
        ErrorLog << "Compaction error: " << status.ToString();
    }

    if (is_manual) {
        ManualCompaction* m = manual_compaction_;
        if (!status.ok()) {
            m->done = true;
        }
        if (!m->done) {
            // 只压缩了部分请求范围，更新 *m 为剩余范围
            m->tmp_storage = manual_end;
            m->begin = &m->tmp_storage;
        }
        manual_compaction_ = nullptr;
    }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
    if (compact->builder != nullptr) {
        // 在压缩中间收到关闭信号时可能发生
        compact->builder->Abandon();
        delete compact->builder;
    } else {
        assert(compact->outfile == nullptr);
    }

    delete compact->outfile;
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        const CompactionState::Output& out = compact->outputs[i];
        // 从待输出的文件集合中移除压缩产生的输出文件
        pending_outputs_.erase(out.sst_number);
    }
    delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
    assert(compact != nullptr);
    assert(compact->builder == nullptr);
    uint64_t sst_number;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        sst_number = versions_->NewFileNumber();
        pending_outputs_.insert(sst_number);
        CompactionState::Output out;
        out.sst_number = sst_number;
        out.smallest_k.Clear();
        out.largest_k.Clear();
        compact->outputs.push_back(out);
    }

    // 创建输出文件
    std::string fname = SSTFileName(dbname_, sst_number);
    Status s = NewWritableFile(fname, &compact->outfile);
    if (s.ok()) {
        compact->builder = new SSTBuilder(compact->outfile);
    }
    return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input) {
    assert(compact != nullptr);
    assert(compact->outfile != nullptr);
    assert(compact->builder != nullptr);

    const uint64_t output_number = compact->current_output()->sst_number;
    assert(output_number != 0);

    // 检查迭代器状态
    Status s = input->status();
    const uint64_t current_entries = compact->builder->EntriesNum();
    if (s.ok()) {
        s = compact->builder->Finish();
    } else {
        compact->builder->Abandon();
    }

    const uint64_t current_bytes = compact->builder->FileSize();
    compact->current_output()->sst_size = current_bytes;
    compact->total_bytes += current_bytes;
    delete compact->builder;
    compact->builder = nullptr;

    // 完成并检查文件错误
    if (s.ok()) {
        s = compact->outfile->Sync();
    }
    if (s.ok()) {
        s = compact->outfile->Close();
    }
    delete compact->outfile;
    compact->outfile = nullptr;

    if (s.ok() && current_entries > 0) {
        Iterator* iter = sst_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
        s = iter->status();
        delete iter;
        if (s.ok()) {
            InfoLog << std::format("Generated sst #{}@{}: {} keys, {} bytes", (unsigned long long)output_number,
                                   compact->compaction->level(), (unsigned long long)current_entries,
                                   (unsigned long long)current_bytes);
        }
    }
    return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
    InfoLog << std::format("Compacted {}@{} + {}@{} files => {} bytes", compact->compaction->input_files_num(0),
                           compact->compaction->level(), compact->compaction->input_files_num(1),
                           compact->compaction->level() + 1, static_cast<long long>(compact->total_bytes));

    // 添加压缩输出文件并删除输入文件
    compact->compaction->AddInputDeletions(compact->compaction->edit());
    const int level = compact->compaction->level();
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        const CompactionState::Output& out = compact->outputs[i];
        compact->compaction->edit()->AddSST(level + 1, out.sst_number, out.sst_size, out.smallest_k, out.largest_k);
        InfoLog << std::format("[DEBUG-COMPACT] Adding output file #{} to L{}: smallest={}, largest={}", out.sst_number,
                               level + 1, out.smallest_k.user_key(), out.largest_k.user_key());
    }
    return versions_->LogAndApply(compact->compaction->edit(), &mtx_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
    const uint64_t start_micros = NowMicros();
    int64_t imm_micros = 0;  // 用于 imm_ 压缩的微妙数

    InfoLog << std::format("Compacting {}@{} + {}@{} files", compact->compaction->input_files_num(0),
                           compact->compaction->level(), compact->compaction->input_files_num(1),
                           compact->compaction->level() + 1);

    assert(versions_->SSTNumOfLevel(compact->compaction->level()) > 0);
    assert(compact->builder == nullptr);
    assert(compact->outfile == nullptr);
    if (snapshots_.empty()) {
        compact->smallest_snapshot = versions_->LastSequence();
    } else {
        compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
    }

    // 创建合并输入迭代器
    Iterator* input = versions_->MakeInputIterator(compact->compaction);

    mtx_.unlock();

    input->SeekToFirst();
    Status status;
    ParsedInternalKey ikey;
    std::string current_user_key;
    bool has_current_user_key = false;
    SequenceNumber last_sequence_for_key = kMaxSequenceNumber;

    while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
        // 优先处理 imm_ 的压缩工作
        if (has_imm_.load(std::memory_order_relaxed)) {
            const uint64_t imm_start = NowMicros();
            mtx_.lock();
            if (imm_ != nullptr) {
                CompactMemTable();
                // 必要时唤醒 MakeRoomForWrite
                background_work_finished_signal_.SignalAll();
            }
            mtx_.unlock();
            imm_micros += (NowMicros() - imm_start);
        }

        std::string_view key = input->key();

        // 如果需要在输出文件边界停止，先完成当前输出文件
        if (compact->compaction->ShouldStopBefore(key) && compact->builder != nullptr) {
            status = FinishCompactionOutputFile(compact, input);
            if (!status.ok()) {
                break;
            }
        }

        // 处理键值对
        bool drop = false;
        if (!ParseInternalKey(key, &ikey)) {
            // 解析失败
            current_user_key.clear();
            has_current_user_key = false;
            last_sequence_for_key = kMaxSequenceNumber;
        } else {
            if (!has_current_user_key ||
                user_comparator()->Compare(ikey.user_key, std::string_view(current_user_key)) != 0) {
                // 第一次出现这个用户键
                current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
                has_current_user_key = true;
                last_sequence_for_key = kMaxSequenceNumber;
            }

            // 决定是否丢弃该条目
            if (last_sequence_for_key <= compact->smallest_snapshot) {
                drop = true;
            } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
                       compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
                // 对于这个用户键：
                // (1) 更高层级中没有数据
                // (2) 更低层级的数据将有更大的序列号
                // (3) 正在这里压缩且序列号更小的数据将在
                //     接下来几次循环中被丢弃（由上面的规则 (A)）
                // 因此这个删除标记已过时，可以丢弃
                drop = true;
            }

            last_sequence_for_key = ikey.sequence;
        }

        if (!drop) {
            // 必要时打开输出文件(替换输出文件时)
            if (compact->builder == nullptr) {
                status = OpenCompactionOutputFile(compact);
                if (!status.ok()) {
                    break;
                }
            }
            if (compact->builder->EntriesNum() == 0) {
                compact->current_output()->smallest_k.DecodeFrom(key);
            }
            compact->current_output()->largest_k.DecodeFrom(key);
            compact->builder->Add(key, input->value());

            // 如果输出文件足够大，关闭它
            if (compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize()) {
                status = FinishCompactionOutputFile(compact, input);
                if (!status.ok()) {
                    break;
                }
            }
        }

        input->Next();
    }

    if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
        status = Status::IOError("Deleting DB during compaction");
    }
    if (status.ok() && compact->builder != nullptr) {
        status = FinishCompactionOutputFile(compact, input);
    }
    if (status.ok()) {
        status = input->status();
    }
    delete input;
    input = nullptr;

    // 更新统计信息
    CompactionStats stats;
    stats.micros = NowMicros() - start_micros - imm_micros;  // compact sst 耗时
    for (int which = 0; which < 2; which++) {
        for (int i = 0; i < compact->compaction->input_files_num(which); i++) {
            // 累加合并涉及的各层输入文件大小
            stats.bytes_read += compact->compaction->get_input_file(which, i)->sst_size;
        }
    }
    for (size_t i = 0; i < compact->outputs.size(); i++) {
        // 累加输出文件大小
        stats.bytes_written += compact->outputs[i].sst_size;
    }

    mtx_.lock();
    // 将本次合并的统计数据累加到总的统计区
    stats_[compact->compaction->level() + 1].Add(stats);

    if (status.ok()) {
        status = InstallCompactionResults(compact);
    }
    if (!status.ok()) {
        RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    InfoLog << "compacted to: " << versions_->LevelSummary(&tmp);
    return status;
}

namespace {

// 迭代器状态，用于清理
struct IterState {
    std::mutex* mtx;
    Version* const version;
    MemTable* const mem;
    MemTable* const imm;

    IterState(std::mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
        : mtx(mutex), version(version), mem(mem), imm(imm) {}
};

// 清理迭代器状态的回调函数，释放 IterState 的所有资源
static void CleanupIteratorState(void* arg1, void* /*arg2*/) {
    IterState* state = reinterpret_cast<IterState*>(arg1);
    state->mtx->lock();
    state->mem->Unref();
    if (state->imm != nullptr) state->imm->Unref();
    state->version->Unref();
    state->mtx->unlock();
    delete state;
}

}  // namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options, SequenceNumber* latest_snapshot, uint32_t* seed) {
    std::lock_guard<std::mutex> lock(mtx_);
    *latest_snapshot = versions_->LastSequence();

    // 收集所有需要的子迭代器
    std::vector<Iterator*> list;
    list.push_back(mem_->NewIterator());

    mem_->Ref();
    if (imm_ != nullptr) {
        list.push_back(imm_->NewIterator());
        imm_->Ref();
    }

    versions_->current()->AddIterators(options, &list);
    Iterator* internal_iter = NewMergeIterator(&internal_comparator_, &list[0], list.size());
    versions_->current()->Ref();

    IterState* cleanup = new IterState(&mtx_, mem_, imm_, versions_->current());
    internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

    *seed = ++seed_;

    return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
    SequenceNumber ignored;
    uint32_t ignored_seed;
    return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
    std::lock_guard<std::mutex> lock(mtx_);
    return versions_->MaxNextLevelOverlapBytes();
}

Status DBImpl::Get(const ReadOptions& options, const std::string_view& key, std::string* value) {
    std::lock_guard<std::mutex> lock(mtx_);

    Status s;
    SequenceNumber snapshot;
    if (options.snapshot != nullptr) {
        snapshot = static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
    } else {
        snapshot = versions_->LastSequence();
    }

    MemTable* mem = mem_;
    MemTable* imm = imm_;
    Version* current = versions_->current();
    // 增加引用计数，防止在读取过程中被删除
    mem->Ref();
    if (imm != nullptr) imm->Ref();
    current->Ref();

    bool have_stat_update = false;
    Version::GetStats stats;

    // 从文件和 MemTable 读取时释放锁
    {
        mtx_.unlock();
        LookupKey lkey(key, snapshot);
        InfoLog << "Get: memtable entries=" << mem->ApproximateMemoryUsage()
                << (imm ? " imm=" + std::to_string(imm->ApproximateMemoryUsage()) : "");
        // 逐级查找
        if (mem->Get(lkey, value, &s)) {
            InfoLog << "Get: found in mem_ key=" << key << " value=" << *value;
        } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
            InfoLog << "Get: found in imm_ key=" << key << " value=" << *value;
        } else {
            InfoLog << "Get: searching SST for key=" << key;
            s = current->Get(options, lkey, value, &stats);
            if (s.ok()) {
                InfoLog << "Get: found in SST key=" << key << " value=" << *value;
            } else {
                InfoLog << "Get: NOT found in SST key=" << key << " err=" << s.ToString();
            }
            have_stat_update = true;
        }
        mtx_.lock();
    }

    // 如果统计信息更新且需要触发压缩
    if (have_stat_update && current->UpdateStats(stats)) {
        MaybeScheduleCompaction();
    }

    mem->Unref();
    if (imm != nullptr) imm->Unref();
    current->Unref();

    return s;
}

Status DBImpl::Put(const WriteOptions& options, const std::string_view& key, const std::string_view& value) {
    return DB::Put(options, key, value);
}

Status DB::Put(const WriteOptions& options, const std::string_view& key, const std::string_view& value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, const std::string_view& key) { return DB::Delete(options, key); }

Status DB::Delete(const WriteOptions& options, const std::string_view& key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(options, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
    WriteInfo w_info(&mtx_);
    w_info.batch = updates;
    w_info.sync = options.sync;
    w_info.done = false;

    std::lock_guard<std::mutex> lock(mtx_);
    writers_info_.push_back(&w_info);
    // 等待成为写队列的头部
    while (!w_info.done && &w_info != writers_info_.front()) {
        w_info.cv.Wait();
    }
    if (w_info.done) {
        return w_info.status;
    }

    // 可能需要临时解锁并等待
    Status status = MakeRoomForWrite(updates == nullptr);
    uint64_t last_sequence = versions_->LastSequence();
    WriteInfo* last_write_info = &w_info;
    if (status.ok() && updates != nullptr) {
        // 构建批处理组（可能包含多个写请求）
        WriteBatch* write_batch = BuildBatchGroup(&last_write_info);
        WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
        last_sequence += WriteBatchInternal::Count(write_batch);

        // 添加到 wal 并应用到 MemTable
        {
            mtx_.unlock();
            status = wal_writer_->AddRecord(WriteBatchInternal::Contents(write_batch));
            bool sync_error = false;
            if (status.ok() && options.sync) {
                status = wal_file_->Sync();  // 刷盘到 wal_file
                if (!status.ok()) {
                    sync_error = true;
                }
            }
            if (status.ok()) {
                // 应用到 mem
                status = WriteBatchInternal::InsertInto(write_batch, mem_);
            }
            mtx_.lock();
            if (sync_error) {
                RecordBackgroundError(status);
            }
        }
        if (write_batch == tmp_batch_) tmp_batch_->Clear();
        versions_->SetLastSequence(last_sequence);
    }

    // 通知所有组内成员写操作完成
    while (true) {
        WriteInfo* ready = writers_info_.front();
        writers_info_.pop_front();
        if (ready != &w_info) {
            ready->status = status;
            ready->done = true;
            ready->cv.Signal();
        }
        if (ready == last_write_info) break;
    }

    // 通知写队列的新头部
    if (!writers_info_.empty()) {
        writers_info_.front()->cv.Signal();
    }

    return status;
}

WriteBatch* DBImpl::BuildBatchGroup(WriteInfo** last_writer) {
    assert(!writers_info_.empty());

    // 将多个写请求合并成一个批量写入，提高吞吐量
    WriteInfo* first = writers_info_.front();
    WriteBatch* result = first->batch;
    assert(result != nullptr);

    size_t size = WriteBatchInternal::ByteSize(first->batch);

    // 允许组增长到最大大小，但如果原始写入较小，限制增长速度，避免拖慢小写入
    size_t max_size = 1 << 20;
    if (size <= (128 << 10)) {
        max_size = size + (128 << 10);
    }

    *last_writer = first;
    std::deque<WriteInfo*>::iterator iter = writers_info_.begin();
    iter++;
    for (; iter != writers_info_.end(); iter++) {
        WriteInfo* w = *iter;
        if (w->sync && !first->sync) {
            // 不要将同步写入包含在非同步写入处理的批处理中
            break;
        }

        if (w->batch != nullptr) {
            size += WriteBatchInternal::ByteSize(w->batch);
            if (size > max_size) {
                // 不要使批处理太大
                break;
            }

            // 追加到 *result
            if (result == first->batch) {
                // 切换到临时批处理，而不是干扰调用者的批处理
                result = tmp_batch_;
                assert(WriteBatchInternal::Count(result) == 0);
                WriteBatchInternal::Append(result, first->batch);
            }
            WriteBatchInternal::Append(result, w->batch);
        }
        *last_writer = w;
    }
    return result;
}

Status DBImpl::MakeRoomForWrite(bool force) {
    assert(!writers_info_.empty());
    bool allow_delay = !force;
    Status s;
    while (true) {
        if (!bg_error_.ok()) {
            // 产生先前的错误
            s = bg_error_;
            break;
        } else if (allow_delay && versions_->SSTNumOfLevel(0) >= gDBConfig->l0_slowdown_writes_trigger) {
            // 此时接近达到 L0 文件数量的硬限制
            // 与其在达到硬限制时将单次写入延迟几秒，不如开始将每次单独写入延迟 1ms 以减少延迟方差
            // 此外，这个延迟将一些 CPU 让给压缩线程，以防它与写入者共享同一个核心
            mtx_.unlock();
            SleepForMicroseconds(1000);
            allow_delay = false;
            mtx_.lock();
        } else if (!force && (mem_->ApproximateMemoryUsage() <= gDBConfig->write_buffer_size)) {
            // 当前活跃 MemTable 有空间
            break;
        } else if (imm_ != nullptr) {
            // 活跃 mem 已经填满，但前一个仍在压缩，所以等待
            InfoLog << "Current memtable full; waiting...";
            background_work_finished_signal_.Wait();
        } else if (versions_->SSTNumOfLevel(0) >= gDBConfig->l0_stop_writes_trigger) {
            // Level-0 文件太多了，停止写入等待压缩
            InfoLog << "Too many L0 files; waiting...";
            background_work_finished_signal_.Wait();
        } else {
            // 尝试切换到新的 MemTable 并触发旧 MemTable 的压缩
            // WAL 文件也要同步更换
            assert(versions_->PrevWalNumber() == 0);
            uint64_t new_wal_number = versions_->NewFileNumber();
            WritableFile* lfile = nullptr;
            s = NewWritableFile(WalFileName(dbname_, new_wal_number), &lfile);
            if (!s.ok()) {
                // 避免在紧循环中消耗文件编号空间
                versions_->ReuseFileNumber(new_wal_number);
                break;
            }

            delete wal_writer_;

            s = wal_file_->Close();
            if (!s.ok()) {
                // 可能丢失了写入前一个日志文件的一些数据
                // 无论如何都要切换到新的日志文件，但记录后台错误，这样我们不会再尝试任何写入
                RecordBackgroundError(s);
            }
            delete wal_file_;

            // wal 换新
            wal_file_ = lfile;
            wal_file_number_ = new_wal_number;
            wal_writer_ = new Writer(lfile);
            imm_ = mem_;
            has_imm_.store(true, std::memory_order_release);
            mem_ = new MemTable(internal_comparator_);
            mem_->Ref();
            force = false;  // 如果有空间，不强制另一次压缩
            MaybeScheduleCompaction();
        }
    }
    return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
    SequenceNumber latest_snapshot;
    uint32_t seed;
    Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
    return NewDBIterator(
        this, user_comparator(), iter,
        (options.snapshot != nullptr ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
                                     : latest_snapshot),
        seed);
}

void DBImpl::RecordReadSample(std::string_view key) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (versions_->current()->RecordReadSample(key)) {
        MaybeScheduleCompaction();
    }
}

const Snapshot* DBImpl::GetSnapshot() {
    std::lock_guard<std::mutex> lock(mtx_);
    return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
    std::lock_guard<std::mutex> lock(mtx_);
    snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

bool DBImpl::GetProperty(const std::string_view& property, std::string* value) {
    value->clear();

    std::lock_guard<std::mutex> lock(mtx_);
    std::string_view in = property;
    std::string_view prefix("delta.");
    if (!in.starts_with(prefix)) return false;
    in.remove_prefix(prefix.size());

    if (in.starts_with("num-files-at-level")) {
        in.remove_prefix(strlen("num-files-at-level"));
        uint64_t level;
        bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
        if (!ok || static_cast<int>(level) >= gDBConfig->num_levels) {
            return false;
        } else {
            *value = std::format("{}", versions_->SSTNumOfLevel(static_cast<int>(level)));
            return true;
        }
    } else if (in == "stats") {
        value->append(
            std::format("          Compactions\n Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n "
                        "--------------------------------------------------\n"));
        for (int level = 0; level < gDBConfig->num_levels; level++) {
            int files = versions_->SSTNumOfLevel(level);
            if (stats_[level].micros > 0 || files > 0) {
                value->append(std::format("{:3d} {:8d} {:8.0f} {:9.0f} {:8.0f} {:9.0f}\n", level, files,
                                          versions_->SSTNumOfLevel(level) / 1048576.0, stats_[level].micros / 1e6,
                                          stats_[level].bytes_read / 1048576.0,
                                          stats_[level].bytes_written / 1048576.0));
            }
        }
        return true;
    } else if (in == "sstables") {
        *value = "没实现";
        return true;
    } else if (in == "approximate-memory-usage") {
        size_t total_usage = gDBConfig->block_cache->TotalCharge();
        if (mem_) {
            total_usage += mem_->ApproximateMemoryUsage();
        }
        if (imm_) {
            total_usage += imm_->ApproximateMemoryUsage();
        }
        value->append(std::format("{}", static_cast<unsigned long long>(total_usage)));
        return true;
    }

    return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
    std::lock_guard<std::mutex> lock(mtx_);
    Version* v = versions_->current();
    v->Ref();

    for (int i = 0; i < n; i++) {
        // 将用户键转换为内部键
        InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
        uint64_t start = versions_->ApproximateOffsetOf(v, k1);  // k1 在数据库中的偏移量
        uint64_t limit = versions_->ApproximateOffsetOf(v, k2);  // k2 在数据库中的偏移量
        sizes[i] = (limit >= start ? limit - start : 0);
    }

    v->Unref();
}

DB::~DB() = default;

Status DB::Open(const std::string& dbname, DB** dbptr) {
    *dbptr = nullptr;

    DBImpl* impl = new DBImpl(dbname);
    impl->mtx_.lock();
    VersionEdit edit;
    // 恢复处理 create_if_missing 和 error_if_exists
    bool save_manifest = false;
    Status s = impl->Recover(&edit, &save_manifest);
    if (s.ok() && impl->mem_ == nullptr) {
        // 创建新的预写日志和对应的 MemTable
        uint64_t new_wal_number = impl->versions_->NewFileNumber();
        WritableFile* lfile;
        s = NewWritableFile(WalFileName(dbname, new_wal_number), &lfile);
        if (s.ok()) {
            edit.SetWalNumber(new_wal_number);
            impl->wal_file_ = lfile;
            impl->wal_file_number_ = new_wal_number;
            impl->wal_writer_ = new Writer(lfile);
            impl->mem_ = new MemTable(impl->internal_comparator_);
            impl->mem_->Ref();
        }
    }
    if (s.ok() && save_manifest) {
        edit.SetPrevWalNumber(0);  // 恢复后不需要旧日志
        edit.SetWalNumber(impl->wal_file_number_);
        s = impl->versions_->LogAndApply(&edit, &impl->mtx_);
    }
    if (s.ok()) {
        impl->RemoveObsoleteFiles();
        impl->MaybeScheduleCompaction();
    }
    impl->mtx_.unlock();
    if (s.ok()) {
        assert(impl->mem_ != nullptr);
        *dbptr = impl;
    } else {
        delete impl;
    }
    return s;
}

Snapshot::~Snapshot() = default;

/**
 * @brief 销毁数据库。删除数据库的所有文件
 */
Status DestroyDB(const std::string& dbname) {
    std::vector<std::string> filenames;
    Status result = GetChildren(dbname, &filenames);
    if (!result.ok()) {
        return Status::OK();
    }

    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type) && type != kDBLockFile) {
            Status del = RemoveFile(dbname + "/" + filenames[i]);
            if (result.ok() && !del.ok()) {
                result = del;
            }
        }
    }
    RemoveDir(dbname);
    return result;
}

}  // namespace delta
