#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <fstream>
#include <memory>
#include <mutex>

class FdCache {
public:
    FdCache(size_t capacity = 4) : max_capacity(capacity) {}

    ~FdCache() {
        // 所有 ifstream 会自动 close
        fd_map.clear();
    }

    // 获取一个可复用的 ifstream（只读）
    std::shared_ptr<std::ifstream> get(const std::string& fname) {
        std::lock_guard<std::mutex> lock(fd_mutex);

        if (fd_map.count(fname)) {
            touch(fname);
            auto fp = fd_map[fname];

            // 修复点：复用之前必须清状态、重置读指针
            fp->clear();    // 清除 failbit/eofbit
            fp->seekg(0);   // 从头读取

            return fp;
        }

        auto fp = std::make_shared<std::ifstream>(fname, std::ios::in);
        if (!fp->is_open()) {
            return nullptr;
        }

        if (fd_map.size() >= max_capacity) {
            std::string old = lru_list.back();
            lru_list.pop_back();
            fd_map.erase(old);
        }

        lru_list.push_front(fname);
        fd_map[fname] = fp;
        return fp;
    }

    void close(const std::string& fname) {
        std::lock_guard<std::mutex> lock(fd_mutex);
        if (fd_map.count(fname)) {
            fd_map.erase(fname);
            lru_list.remove(fname);
        }
    }

    void clear() {
        fd_map.clear();
    }

private:
    void touch(const std::string& fname) {
        lru_list.remove(fname);
        lru_list.push_front(fname);
    }

    size_t max_capacity;
    std::unordered_map<std::string, std::shared_ptr<std::ifstream>> fd_map;
    std::list<std::string> lru_list;
    std::mutex fd_mutex;
};
