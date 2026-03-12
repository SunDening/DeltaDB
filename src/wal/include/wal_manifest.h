#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <iostream>
#include <algorithm>

class WalManifest {
public:
    std::string manifest_dir_;

    explicit WalManifest(const std::string& manifest_dir_);

    // 加载 manifest 文件内容（程序启动时调用）
    bool load();

    // 生成下一个待归档的wal_name
    std::string get_next_wal_name();

    // 添加一个新的未刷盘 WAL 文件（刷盘前调用）
    void add_pending_wal(const std::string& wal_name);

    // 删除已经刷盘成功的 WAL 文件（刷盘后调用）
    void remove_wal(const std::string& wal_name);

    // 获取当前还未刷盘的所有 WAL 文件（用于恢复）
    std::vector<std::string> get_pending_wals() const;

private:
    bool rewrite_manifest(); // 原子写入 manifest 文件

    
    std::string manifest_path_;
    std::unordered_set<std::string> pending_wals_;
    mutable std::mutex mutex_;
};
