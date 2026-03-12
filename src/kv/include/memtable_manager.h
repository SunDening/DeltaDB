#pragma once

#include <deque>
#include <memory>
#include <string>
#include <mutex>
#include "memtable.h"  // 你已有的 MemTable 实现

struct ImmutableEntry {
    std::shared_ptr<MemTable> mem;
    std::string wal_filename;
};

class MemTableManager {
public:
    void add_immutable(std::shared_ptr<MemTable>& mem, const std::string& wal_name);
    bool has_immutable();
    ImmutableEntry get_oldest_immutable();
    void pop_oldest_immutable();
    // 按由新到旧的顺序返回
    vector<shared_ptr<MemTable>> get_immutable_list_desc();

private:
    std::mutex mutex_;
    std::deque<ImmutableEntry> immutable_queue_;
};
