#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>

#include "db.h"
#include "dbformat.h"
#include "log.h"
#include "single_thread_pool.h"
#include "snapshot.h"
#include "wal_writer.h"

namespace delta {

extern delta::Config::ptr gDBConfig;
extern delta::Logger::ptr gDBLogger;
class MemTable;
class SSTCache;
class Version;
class VersionEdit;
class VersionSet;

/**
 * 数据库核心实现类
 *
 * DB (公共接口，抽象类)
    ↑
    │ 继承实现
    │
DBImpl (实际实现类)
    │
    ├── 写路径：Put/Delete/Write → WAL → MemTable
    ├── 读路径：Get/NewIterator → MemTable + SST
    ├── 后台线程：Compaction
    └── 版本管理：VersionSet
 *
 *
 *
 */
class DBImpl : public DB {
   private:
    friend class DB;

    struct CompactionState;

    struct WriteInfo;

    // ============================================================================
    // 内部结构体：手动 Compaction 参数
    // ===========================================================================

    /**
     * 手动 Compaction 的控制参数
     * 后台 Compaction 线程处理该请求，完成后设置 done = true 并通知主线程
     */
    struct ManualCompaction {
        int level;                 // 要 Compaction 的层级（-1 表示自动选择）
        bool done;                 // 完成标志
        const InternalKey* begin;  // 范围起始（nullptr 表示从头开始）
        const InternalKey* end;    // 范围结束（nullptr 表示到末尾）
        InternalKey tmp_storage;   // 临时存储：用于跟踪压实进度
    };

    // ============================================================================
    // 内部结构体：Compaction 统计信息
    // ============================================================================

    /**
     * 每个层级的 Compaction 统计数据
     */
    struct CompactionStats {
        int64_t micros;         // 耗时（微秒）
        int64_t bytes_read;     // 读取字节数
        int64_t bytes_written;  // 写入字节数

        /**
         * 累加另一个统计对象的数据
         * 用途：合并多次 Compaction 的统计数据
         */
        void Add(const CompactionStats& c) {
            this->micros += c.micros;
            this->bytes_read += c.bytes_read;
            this->bytes_written += c.bytes_written;
        }

        CompactionStats() : micros(0), bytes_read(0), bytes_written(0) {}
    };

    // ============================================================================
    // 常量成员（构造后不变）可安全读取（无需加锁）
    // ============================================================================
    // Env* const env_;                              // 环境抽象层（文件、时间、调度等）
    const InternalKeyComparator internal_comparator_;    // 内部键比较器（含 user_key 比较器）
    const InternalFilterPolicy internal_filter_policy_;  // 内部过滤器策略（包装用户过滤器）
    // const Options options_;  // options_.comparator == &internal_comparator_
    const std::string dbname_;  // 数据库路径名称

    SSTCache* const sst_cache_;  // table_cache_ 提供自己的同步机制，访问时无需持有 mutex_

    // FileLock* db_lock_;  //  数据库文件锁，成功获取后非 null. 用途：防止同一数据库被多个进程同时打开

    // ============================================================================
    // 受 mutex_ 保护的状态变量
    // ============================================================================

    std::mutex mtx_;                           // 全局状态保护锁
    std::atomic<bool> shutting_down_;          // 关闭标志。 使用 atomic 以便快速检测，无需持锁
    CondVar background_work_finished_signal_;  // 后台工作完成信号量

    MemTable* mem_;              // 当前活跃的 MemTable（接收写入）
    MemTable* imm_;              // 冻结的 MemTable（等待 compact 到刷盘）
    std::atomic<bool> has_imm_;  // imm_ 非空的快速检测标志
    WritableFile* wal_file_;     // 当前 WAL 日志文件
    uint64_t wal_file_number_;   // 当前 WAL 文件序号
    Writer* wal_writer_;         // WAL 日志写入器
    uint32_t seed_;              // 随机种子，用于读取采样

    std::deque<WriteInfo*> writers_info_;  // Writer 队列：等待写入的客户端. 尾入头出
    WriteBatch* tmp_batch_;                // 临时 batch，用于批量合并

    SnapshotList snapshots_;  // 活跃快照双向链表

    std::set<uint64_t> pending_outputs_;  // 待输出的文件集合（Compaction 进行中）。防止这些文件在 compact 完成前被删除

    bool background_compaction_scheduled_;  // 后台 Compaction 调度标志

    ManualCompaction* manual_compaction_;  // 手动 Compaction 请求指针

    VersionSet* const versions_;  // 版本管理器（LSM 树的核心数据结构）。管理所有 SST 文件的元数据和层级信息

    Status bg_error_;  // 后台错误状态（paranoid_checks 模式下）。非 OK 时表示后台发生错误，后续操作会失败

    CompactionStats stats_[7];  // 每个层级的 Compaction 指针

    SingleThreadThreadPool thread_pool;  // 单线程线程池，用于后台任务调度

    // ============================================================================
    // 内部迭代器创建
    // ============================================================================

    /**
     * @brief 创建内部迭代器
     * @param latest_snapshot：输出，最新快照的序列号
     * @param seed：输出，随机种子（用于采样）
     *
     * 构建覆盖所有数据源的合并迭代器，返回的迭代器使用 InternalKey（含序列号和类型）
     * 数据源包括：
     *  - 当前 MemTable（mem_）
     *  - 冻结 MemTable （imm_）
     *  - L0 层所有 SST 文件
     *  - L1-Ln 层的合并迭代器（每层一个）
     */
    Iterator* NewInternalIterator(const ReadOptions&, SequenceNumber* latest_snapshot, uint32_t* seed);

    // ============================================================================
    // 数据库初始化
    // ============================================================================

    /**
     * @param 创建新数据库
     * 用途：
     *  - create_if_missing=true 且数据库不存在时调用
     *  - 创建初始 MANIFEST 文件
     *  - 创建当前版本编辑（VersionEdit）并保存
     */
    Status NewDB();

    // ============================================================================
    // 数据库恢复
    // ============================================================================

    /**
     * @brief 从 MANIFEST 和日志文件中恢复数据库状态
     * @param edit：输出，对描述符的修改
     * @param save_manifest：输出，是否需要保存 MANIFEST
     *
     * 恢复流程：
     *  1. 读取 CURRENT 文件 -> 获取当前 MANIFEST 文件名
     *  2. 读取 MANIFEST 文件 -> 恢复 VersionSet
     *  3. 扫描所有 SST 文件 -> 构建版本信息
     *  4. 按序号回放 WAL 日志 -> 恢复 MemTable
     *  5. 清理过期文件
     *
     * 可能做大量工作：回放最近的日志更新
     */
    Status Recover(VersionEdit* edit, bool* save_manifest);

    /**
     * @brief 忽略某些非关键错误
     * 用途：
     *  - 在非 paranoid_checks（严格检查） 模式下，某些错误可以忽略
     *  - 例如：文件损坏但不影响核心数据
     */
    void MaybeIgnoreError(Status* s) const;

    /**
     * @brief 删除不需要的文件和过期的内存条目
     * 清理对象：
     *  1. 过期的 WAL（已 compact 到 SST）
     *  2. 过期的 SST （不再属于任何版本）
     *  3. 临时的 Compaction 输出文件
     *  4. 已释放的 MemTable
     */
    void RemoveObsoleteFiles();

    // ============================================================================
    // MemTable Compaction
    // ============================================================================

    /**
     * @brief 将 MemTable 紧凑到磁盘
     * 执行操作：
     *  1. 将 imm_ （冻结的 MemTable）写入 L0 层 SST 文件
     *  2. 切换到新的 log-file/memtable
     *  3. 如果成功，写入新的描述符（MANIFEST）
     */
    void CompactMemTable();

    /**
     * @brief 从 WAL 日志文件恢复数据
     * 参数：
     * @param wal_number：wal 文件序号
     * @param last_log：是否是最后一个日志文件
     * @param save_manifest：输出，是否需要保存 MANIFEST
     * @param edit：输出，编辑版本信息
     * @param max_sequence：输出，最大序列号
     *
     * 恢复流程：
     *  1. 打开指定序号的 wal 文件
     *  2. 逐条读取 WriteBatch 记录
     *  3. 应用到 MemTable
     *  4. 更新最大序列号
     */
    Status RecoverWalFile(uint64_t wal_number, bool last_log, bool* save_manifest, VersionEdit* edit,
                          SequenceNumber* max_sequence);

    /**
     * @brief 将 MemTable 写入 L0 层 SST 文件
     * @param mem：要 compact 的 MemTable
     * @param edit：输出，版本编辑信息（记录新添加的文件）
     * @param base：基础版本（用于计算重叠）
     *
     * L0 层特殊性：
     *  - L0 层文件之间可能有 key 重叠
     *  - L0 文件数过多会触发 Compaction
     */
    Status WriteToLevel0(MemTable* mem, VersionEdit* edit, Version* base);

    // ============================================================================
    // 写路径控制
    // ============================================================================

    /**
     * @brief 为写入操作腾出空间
     * @param force：是否强制 compact（即使还有空间）
     *
     * 检查项：
     *  1. mem_ 是否已满 -> 是则切换到 imm
     *  2. imm_ 是否非空 -> 是则等待后台 compact 完成
     *  3. L0 文件数是否过多 -> 是则触发 compact
     *  4. 某层级数据量是否过大 -> 是则触发 compact
     */
    Status MakeRoomForWrite(bool force);

    /**
     * @brief 构建批量写入组
     * @param last_writer：输出，最后一个追加到 Group 的 write_info （并非writers_info_都一定被追加到group）
     * @return WriteBatch*：合并后的 batch
     *
     * 用途：
     *  - 将 writers_info_ 队列中的多个写入请求合并
     *  - 减少 wal 和 MemTable 的写入次数
     *  - 提高吞吐量
     *
     * 合并策略：
     *  1. 取队列头部的 writer_info
     *  2. 追加后续 writer_info 的 batch （如果总大小不超过限制）
     *  3. 返回合并后的 batch
     */
    WriteBatch* BuildBatchGroup(WriteInfo** last_writer);

    // ============================================================================
    // 错误处理
    // ============================================================================

    /**
     * @brief 记录后台错误
     *
     * 用途：
     *  - 在 paranoid_checks（严格检查） 模式下，后台错误会导致数据库不可用
     *  - 错误存储在 bg_error_ 中，后续操作会返回该错误
     */
    void RecordBackgroundError(const Status& s);

    // ============================================================================
    // Compaction 调度与执行
    // ============================================================================

    /**
     * @brief 检查并调度后台 Compaction
     *
     * 触发条件：
     *  1. imm_ 非空（MemTable 满）
     *  2. L0 文件数超过阈值
     *  3. 某层级数据量超过阈值
     *  4. 有手动 compact 请求
     */
    void MaybeScheduleCompaction();

    /**
     * @brief 后台工作线程入口函数
     *
     * @param db：DBImpl 实例指针（void* 用于 pthread 兼容）
     *
     * 调用流程：
     *  pthread_create → BGWork → BackgroundCall → BackgroundCompaction
     */
    static void BGWork(void* db);

    /**
     * @brief 后台线程主循环
     *
     * 执行流程：
     *  1. 调用 BackgroundCompaction()
     *  2. 通知等待的线程（background_work_finished_signal_）
     */
    void BackgroundCall();

    /**
     * @brief 执行后台 Compaction，实际的压缩操作
     *
     * Compaction 类型：
     *  1. MemTable Compaction（imm_ → L0）
     *  2. L0 Compaction（L0 → L1）
     *  3. 层级 Compaction（Ln → Ln+1）
     *  4. 手动 Compaction
     *
     * 执行流程：
     *  1. 选择输入文件
     *  2. 创建合并迭代器
     *  3. 读取、合并、去重
     *  4. 写入新的 SST 文件
     *  5. 更新 VersionSet
     *  6. 清理过期文件
     */
    void BackgroundCompaction();

    /**
     * @brief 清理 Compaction 状态
     *
     * @param compact： Compaction 状态对象
     *
     * 清理操作：
     *  1. 关闭输出文件
     *  2. 从 pending_outputs_ 中移除
     *  3. 删除 compact 对象
     */
    void CleanupCompaction(CompactionState* compact);

    /**
     * @brief 执行 Compaction 核心工作循环
     *
     * @param compact：Compaction 状态对象
     *
     * 工作流程：
     *  1. 打开输出文件
     *  2. 创建输入迭代器
     *  3. 逐条读取、合并、去重
     *  4. 写入输出文件
     *  5. 记录统计信息
     *  6. 完成输出文件
     */
    Status DoCompactionWork(CompactionState* compact);

    // ============================================================================
    // Compaction 文件操作
    // ============================================================================

    /**
     * @brief 打开 Compaction 输出文件
     *
     * @param compact：Compaction 状态对象
     *
     * 用途;
     *  - 创建新的 SST 文件
     *  - 文件名格式：{table_number}.sst
     *  - 添加到 pending_outputs_ 保护不被删除
     */
    Status OpenCompactionOutputFile(CompactionState* compact);

    /**
     * @brief 完成 Compaction 输出文件
     *
     * @param compact：Compaction 状态对象
     * @param input：输入迭代器
     *
     * 用途：
     *  - 关闭文件
     *  - 安装到 SSTCache
     *  - 记录文件元数据
     */
    Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);

    /**
     * @brief 安装 Compaction 结果到版本系统
     * @param compact：Compaction 状态对象
     *
     * 操作：
     *  1. 创建 VersionEdit
     *  2. 删除输入文件记录
     *  3. 添加输出文件记录
     *  4. 应用到 VersionSet
     *  5. 保存 MANIFEST
     */
    Status InstallCompactionResults(CompactionState* compact);

    // ============================================================================
    // 比较器访问
    // ============================================================================

    /**
     * @brief 获取用户比较器，用于用户键的比较（不包括序列号和类型）
     */
    const Comparator* user_comparator() const { return internal_comparator_.user_comparator(); }

   public:
    DBImpl(const std::string& dbname);

    DBImpl(const DBImpl&) = delete;
    DBImpl& operator=(const DBImpl&) = delete;

    /// @brief 安全地关闭数据库，等待后台任务完成并释放所有资源
    ~DBImpl() override;

    // ============================================================================
    // 公共接口实现（继承自 DB）
    // ============================================================================
    Status Put(const WriteOptions&, const std::string_view& key, const std::string_view& value) override;
    Status Delete(const WriteOptions&, const std::string_view& key) override;
    /**
     * @brief 写入数据批次，使用写队列实现。
     * 1. 写批处理组（WriteBatch Grouping）- 合并多个写操作
     * 2. 写-ahead 日志（WAL）- 确保持久性
     * 3. 写入 MemTable - 内存数据结构
     */
    Status Write(const WriteOptions& options, WriteBatch* updates) override;
    /**
     * @brief 读取单个键值
     * 按以下顺序查找键：
     *  1. 活动 MemTable
     *  2. 不可变 MemTable（如果有）
     *  3. 所有层级的 SST 文件
     */
    Status Get(const ReadOptions& options, const std::string_view& key, std::string* value) override;

    /**
     * @brief 创建可用于遍历所有键值对的迭代器
     */
    Iterator* NewIterator(const ReadOptions&) override;
    const Snapshot* GetSnapshot() override;
    void ReleaseSnapshot(const Snapshot* snapshot) override;
    /**
     * @brief 获取数据库属性
     * 支持查询：
     *  - num-files-at-level<N>: 某层级的文件数
     *  - stats: 压缩统计
     *  - sstables: 当前版本的调试信息
     *  - approximate-memory-usage: 近似内存使用
     */
    bool GetProperty(const std::string_view& property, std::string* value) override;
    /**
     * @brief 获取键范围内的数据大小
     */
    void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) override;

    // 对给定键范围执行手动压缩
    void CompactRange(const std::string_view* begin, const std::string_view* end) override;

    /**
     * @brief 记录在指定的内部键上读取的字节样本。
     * 大约每 gDBConfig::read_bytes_period 字节采样一次。
     * 用于触发基于访问频率的压缩。
     */
    void RecordReadSample(std::string_view key);

    // compact 指定 level 中与给定范围有重叠的文件
    void TEST_CompactRange(int level, const std::string_view* begin, const std::string_view* end);

    // 强制 compact 当前 memtable （强制刷盘）
    Status TEST_CompactMemTable();

    // 返回数据库当前状态的内部键迭代器
    Iterator* TEST_NewInternalIterator();

    // 返回级别>= 1的任何文件在下一级别的最大重叠数据（以字节为单位）
    int64_t TEST_MaxNextLevelOverlappingBytes();
};

}  // namespace delta
