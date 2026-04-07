#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "log.h"

namespace delta {

class SkipList {
   private:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node *> forward;  // forward[i] 是指向第 i 层中该节点的下一个节点。

        Node(std::string k, std::string v, int level) : key(k), value(v), forward(level, nullptr) {}
    };

    int maxLevel_;       // 最大层数
    float probability_;  // 节点晋升概率
    Node *header_;       // 头节点，指向每层的起点。
    int currentLevel_;   // 当前最大层数

    size_t entry_count_;
    size_t total_size_bytes_;

    std::shared_mutex rw_mtx_;

    // 随机生成节点层数
    // 每个新插入的节点会通过这个函数决定自己的层数。
    // 晋升概率控制了跳表的“稀疏程度”。
    int randomLevel();

   public:
    typedef std::shared_ptr<SkipList> ptr;

    SkipList(int maxLvl = 16, float p = 0.5);

    SkipList(SkipList &&other) noexcept;

    ~SkipList();

    // 插入键值
    bool insert(const std::string &key, const std::string &value);

    // 查找键值
    std::optional<std::string> search(const std::string &key);

    // 删除键值
    bool erase(const std::string &key);

    // 打印跳表结构
    void display();

    size_t size();

    size_t count();

    bool empty();

    std::vector<std::pair<std::string, std::string>> traverse();
};

}  // namespace delta
