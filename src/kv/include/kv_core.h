#pragma once

#include <iostream>
#include <unordered_map>
#include <string>
#include <fstream>
#include <memory>
#include <cstdio>
#include <cmath>
#include <mutex>        // 提供 std::mutex, std::lock_guard, std::unique_lock
#include <shared_mutex>
#include <thread>
#include <condition_variable>  // std::condition_variable
#include <chrono>              // 如果你要用 sleep_for 或 wait_for

#include "memtable.h"
#include "sstable_manager.h"  // manage the manifest.txt file
#include "wal.h"
#include "compaction_manager.h"  // compact 任务管理类
#include "fd_cache.h"  // 文件句柄池

using namespace std;


// 用一个特殊字符串表示删除标记（Tombstone）
// const string TOMBSTONE = "<TOMBSTONE>";

class Little_kv {
private:
    shared_ptr<MemTable> active_memtable;
    // shared_ptr<MemTable> immutable_memtable;  // 改为维护一个<wal_name,immu_mem>队列

    string sst_dir_path;  // only dir path , no file_name
    size_t memtable_size_threshold;  // over the value, datas in memtable will be writen to L0
    size_t compact_threshold;  // if L0 files'num supass the value, will compact to L1
    size_t max_file_size;  // a L1 file should be the size
    size_t per_kv_size;

    WAL& wal;  // 预写日志/日志

    FdCache fd_cache;

    size_t levels_multiple = 4;  // 相邻层次间容量倍数差距（两个用途：初始化bloom filter、合并函数传参）


    // to manage sst files   link to manifest.txt
    shared_ptr<SSTableManager> sst_manager;

    // to manager memtables
    shared_ptr<MemTableManager> mem_manager;


    // mutex
    mutex edit_mutex;  // mutex fot put and del
    shared_mutex rw_manifest_mutex;

    // 线程
    std::thread flush_thread;
    // std::thread compact_thread;

    // 同步
    std::mutex flush_mutex;
    std::condition_variable flush_cv;
    bool flush_needed = false;
    bool stop_flush = false;

    // std::mutex compact_mutex;
    // std::condition_variable compact_cv;
    // bool compact_needed = false;
    // bool stop_compact = false;
    CompactionManager compaction_mgr;
    std::thread detector_thread;
    std::atomic<bool> stop_detector{false};

    // 循环执行检测是否需要合并
    void detector_loop();


    // 初始化，重放WAL恢复
    void Init_data();

    void readin_ssts_names();

    // 刷盘操作
    void Flush();

    bool RemoveSstFiles(FileMetaData& file);
    bool RemoveSstFiles(vector<FileMetaData>& files);
    

public:
    Little_kv(const string& sst_dir_path, const size_t memtable_threshold, const size_t compact_threshold, const size_t max_size, const size_t per_kv_size, WAL& wal);

    ~Little_kv();

    // put操作（包含普通写入和删除）
    bool put(const std::string& key, const std::string& value);

    // 原子性的批量写入，num表示接下来写入的数据数量
    bool putbat(const int num);

    // del操作，实际上写入Tombstone
    // 没有真正“删除”旧数据的操作，所有删除操作本质上是put一个特殊的“删除标记”（Tombstone）值。
    bool del(const std::string& key);

    // get操作
    string get(const string& key);

    // 多线程相关
    // 后台刷盘 & 合并
    void flush_background();

    // 用于触发
    void maybe_trigger_flush();

    // 检测并规划compact （类似于manual_compact()）
    void detect_and_schedule();

    /**
     * 每次选择某层的一个最老文件合并到低一层
    */
    bool CompactLn(int target_level);
    /**
     * L1->L2  L2->L3
     * source_files ：源数据文件，更高一层
     * target_files ：目标数据文件，更低一层
     * target_level ：目标层次
     * target_file_size ：目标文件基本大小，设计上下层文件大小是上层的10倍
     * last_level ：标识是否是合并到最底层，仅在最底层处理删除标记
     * 每次将某高层全部合并到低一层
    */
    bool CompactLnBat(int target_level);


    // 清除所有记录
    void clear();

    // 刷新日志
    void refresh_log();

};