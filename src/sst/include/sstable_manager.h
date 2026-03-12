#pragma once

#include <iostream>
#include <vector>
#include <algorithm>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cmath>
#include <mutex>        // 提供 std::mutex, std::lock_guard, std::unique_lock
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "wal.h"
#include "bloom_filter.h"
#include "fd_cache.h"
#include "snappy_compressor.h"

using namespace std;

struct FileMetaData {
    int level;
    int id;
    string filename;
    string smallest_key;
    string largest_key;
    size_t file_size;  // 当前文件大小
    BloomFilter filter; // 新增字段，每个sst单独拥有一个filter，在生成每个 SST 文件时，把所有 key 加入 filter

    FileMetaData(int& lel, int id, const string& fname, const string& skey, const string& lkey, size_t file_size, const BloomFilter& bf)
        : level(lel), id(id), filename(fname), smallest_key(skey), largest_key(lkey), file_size(file_size), filter(bf) {}
};

// manager all ssts
class SSTableManager {
private:
    string manifest_dir;
    WAL& wal;
    vector<FileMetaData> file_list;
    size_t base_file_size;
    size_t per_kv_size;

    std::thread save_thread;
    std::atomic<bool> stop_thread;
    

public:
    size_t levels_multiple;
    vector<size_t> allow_file_sizes;

    SSTableManager(const string& dir, WAL& wal, size_t base_file_size=32, size_t levels_multiple=4, size_t per_kv_size=8)
     : manifest_dir(dir), wal(wal), base_file_size(base_file_size), levels_multiple(levels_multiple), per_kv_size(per_kv_size) {
        // L0-L3各层允许的最大容量
        allow_file_sizes = {base_file_size, base_file_size, base_file_size*levels_multiple, base_file_size*levels_multiple*levels_multiple};

    };

    bool load();  // load all sst record from manifest.txt to memory
    bool saveToTmp();  // 先将fmds写到一个临时文件，防止执行save中途崩溃
    bool save();  // write all records to manifest.txt
    void addFile(const FileMetaData& meta);  // add a record to minifest.txt
    void removeFile(const string& filename);
    
    const vector<FileMetaData>& getFileList() const;
    vector<FileMetaData> getFilesByLevel(const int& level) const;
    /**
     * 返回该level下id最小的fmd
    */
    FileMetaData getEarliestFileByLevel(const int& level) const;
    size_t getLevelSize(const int& level);
    int getNextSstId(const int& level);
    string getNextSstName(const int& level);
    string getNextSstName(const int& level, const int& id);
    vector<FileMetaData> getRelativeSsts(const int& level, const set<string> &key_set);
    /**
     * 从vector中获取key的范围
    */
    bool getKeyRange(const vector<FileMetaData>& files, string& min_k, string& max_k);
    /**
     * 从map中获取key的范围，忽略value为删除标记的数据
     * 在非目标为最底层的文件合并过程中，删除标记不可忽略，故不能用该函数
     * 适用于目标为最底层的文件的合并过程
    */
    bool getKeyRange_ignore_del(const unordered_map<string, string> data_map, string& min_k, string& max_k);
    /**
     * 从map中获取key的范围，考虑value为删除标记的数据
     * 适用于非目标为最底层的合并过程
    */
    bool getKeyRange_contains_del(const unordered_map<string, string> data_map, string& min_k, string& max_k);

    set<string> getKeySet(const FileMetaData& file);
    set<string> getKeySet(const vector<FileMetaData>& files);

    void readFileToMap(FileMetaData& file, unordered_map<string, string>& data_map);
    void readFileToMap(vector<FileMetaData>& files, unordered_map<string, string>& data_map);

    void writeMapToSst(const unordered_map<string, string>& data_map, const string& filename);

    string searchFromSst(const int& level, const string& key, FdCache& fd_cache);

    // 后台线程部分

    // 启动后台线程
    void startBackgroundSave();

    // 停止后台线程
    void stopBackgroundSave();
};