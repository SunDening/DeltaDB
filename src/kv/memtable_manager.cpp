#include "memtable_manager.h"

void MemTableManager::add_immutable(std::shared_ptr<MemTable>& mem, const std::string& wal_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    immutable_queue_.emplace_back(ImmutableEntry{mem, wal_name});
}

bool MemTableManager::has_immutable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !immutable_queue_.empty();
}

ImmutableEntry MemTableManager::get_oldest_immutable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return immutable_queue_.front();
}

void MemTableManager::pop_oldest_immutable() {
    std::lock_guard<std::mutex> lock(mutex_);
    immutable_queue_.pop_front();
}

vector<shared_ptr<MemTable>> MemTableManager::get_immutable_list_desc() {
    std::lock_guard<std::mutex> lock(mutex_);
    vector<shared_ptr<MemTable>> immutable_list_desc;
    for (const auto entry : immutable_queue_) {
        immutable_list_desc.insert(immutable_list_desc.begin(), entry.mem);
    }
    return immutable_list_desc;
}
