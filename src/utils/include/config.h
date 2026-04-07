#pragma once

#include <tinyxml.h>
#include <format>
#include <map>
#include <memory>
#include <string>

#include "util.h"

namespace delta {

// =============== NEW begin =================
class Cache;
class Comparator;
class FilterPolicy;
class Snapshot;
class Limiter;

/**
 * 资源数量限制器
 *
 * 用途：控制并发资源 (如文件描述符、mmap 区域) 的数量，防止资源耗尽
 *
 * 特点：
 *  - 无锁设计：使用 atomic 操作，无需 mutex
 *  - 线程安全：支持多线程并发 Acquire/Release
 *  - 非阻塞：Acquire() 立即返回，不等待资源
 */
class Limiter {
   public:
    // 初始可用资源数 = 最大值
    Limiter(int max_acquires) : acquires_allowed_(max_acquires) { assert(max_acquires >= 0); }

    Limiter(const Limiter &) = delete;
    Limiter operator=(const Limiter &) = delete;

    /**
     * @brief 尝试获取一个资源
     * @return true：获取成功，可以使用资源；false：资源已耗尽，需降级处理；
     */
    bool Acquire() {
        // 原子减1，尝试获取资源
        int old_acquires_allowed = acquires_allowed_.fetch_sub(1, std::memory_order_relaxed);

        // 减 1 前还有资源 → 获取成功
        if (old_acquires_allowed > 0) return true;

        // 减1后发现资源不足，需要回退，原子加1，恢复原值
        int pre_increment_acquires_allowed = acquires_allowed_.fetch_add(1, std::memory_order_relaxed);

        // 关闭编译器的未使用参数警告
        (void)pre_increment_acquires_allowed;

        return false;
    }

    /**
     * @brief 释放一个之前获取的资源
     *
     * 前提条件：必须与成功的 Acquire() 配对使用
     */
    void Release() {
        // 原子加1，归还资源
        int old_acquires_allowed = acquires_allowed_.fetch_add(1, std::memory_order_relaxed);

        // 关闭编译器的未使用参数警告
        (void)old_acquires_allowed;
    }

   private:
    std::atomic<int> acquires_allowed_;  // 当前可用资源数量
};

/**
 * 压缩算法类型
 */
enum CompressionType {
    kNoCompression = 0x0,
    kSnappyCompression = 0x1,
    kZstdCompression = 0x2,
};
// =============== NEW end =================

class Config {
   private:
    std::string conf_file_path_;
    TiXmlDocument *xml_file_;  // xml文件

   public:
    typedef std::shared_ptr<Config> ptr;

    // 日志参数 (log params)
    std::string log_path_;                    // 路径（目录）
    std::string log_prefix_;                  // 日志名前缀
    int log_max_file_size_;                   // 单日志文件最大容量 MB
    LogLevel db_log_level_{LogLevel::DEBUG};  // DB内部的日志等级
    int log_sync_interval_{500};              // 日志同步间隔

    // db参数（db params）
    std::string db_path;
    int num_levels;                  // 层数
    int l0_compaction_trigger;       // Level-0 有多少个文件时开始压缩
    int l0_slowdown_writes_trigger;  // 0级文件数量软限制。此时我们减慢了写入速度.
    int l0_stop_writes_trigger;      // 0级文件的最大数目。我们在这里停止写入。
    int max_mem_compact_level;       // 如果一个新的压缩memtable不产生重叠，那么它被压入的最大级别。
    int read_bytes_period;           // 迭代期间读取的数据样本之间的近似字节间隔。

    // ================= NEW start ====================

    const Comparator *comparator;           // User Comparator（用户键比较器）
    const Comparator *internal_comparator;  // Internal Key Comparator（内部键比较器）

    CompressionType compression = kSnappyCompression;  // 压缩算法

    const FilterPolicy *filter_policy{nullptr};  // Bloom 过滤器策略

    Cache *block_cache{nullptr};  // 块缓存（默认 8MB）

    // Env* env;  // 环境抽象层

    bool create_if_missing{false};  // 不存在则创建
    bool error_if_exists{false};    // 已存在则报错
    bool paranoid_checks{false};    // 严格检查模式

    size_t write_buffer_size{4 * 1024 * 1024};  // MemTable 大小阈值 4MB
    int max_open_files{1000};                   // 最大打开文件数

    size_t block_size{4 * 1024};            // 块大小 4MB
    int block_restart_internal{16};         // 键重启间隔
    size_t max_file_size{2 * 1024 * 1024};  // SST 文件最大大小 2MB

    int zstd_compression_level{1};  // ZSTD 压缩级别
    bool reuse_logs{false};         // 重用日志文件

    Limiter fd_limiter;

    // ================= NEW end ====================

    Config(const std::string &conf_file_path);

    ~Config();

    void readConf();

    void readLogConfig(TiXmlElement *log_node);

    void readDBConfig(TiXmlElement *db_node);

    TiXmlElement *getXmlNode(const std::string &name);

    void checkType(TiXmlElement *log_node, std::string type);

    void checkItem(TiXmlElement *node, std::string item);
};

/**
 * 读操作级配置
 * 每次读操作时传入，控制单次读取行为
 */
struct ReadOptions {
    bool verify_checksums{false};       // 验证数据校验和
    bool fill_cache{true};              // 是否缓存到内存
    const Snapshot *snapshot{nullptr};  // 指定快照版本
};

/**
 * 写操作级配置
 * 每次写操作时传入，控制单次写入行为
 */
struct WriteOptions {
    WriteOptions() = default;

    bool sync{false};  // 是否 fsync 刷盘
};

}  // namespace delta