#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>  // 提供 std::mutex, std::lock_guard, std::unique_lock
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

// 墓碑数据标志
const std::string TOMBSTONE = "<TOMBSTONE>";

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

    SkipList(int maxLvl = 16, float p = 0.5) : maxLevel_(maxLvl), probability_(p), currentLevel_(1) {
        this->header_ = new Node("", "", maxLevel_);
        entry_count_ = 0;
        total_size_bytes_ = 0;
    }

    ~SkipList() {
        Node *current = header_->forward[0];
        while (current != nullptr) {
            Node *next = current->forward[0];
            delete current;
            current = next;
        }
        delete header_;
    }

    // 插入键值
    bool insert(std::string key, std::string value);

    // 查找键值
    std::optional<std::string> search(std::string key);

    // 删除键值
    bool erase(std::string key);

    // 打印跳表结构
    void display();

    size_t size() const;

    bool empty() const;

    std::vector<std::pair<std::string, std::string>> traverse();

    // 计算当前 active_memtable 大小（简单计数）
    size_t GetMemTableSize();
};

int SkipList::randomLevel() {
    int lvl = 1;
    while ((rand() % 100) < (probability_ * 100) && lvl < maxLevel_) {
        lvl++;
    }
    return lvl;
}

// 插入键值
bool SkipList::insert(std::string key, std::string value) {
    std::unique_lock<std::shared_mutex> memtable_lock(rw_mtx_);

    std::vector<Node *> update(maxLevel_, nullptr);
    Node *current = header_;

    // 从最高层开始查找插入位置
    for (int i = currentLevel_ - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr && current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;  // 记录每层需要更新的节点
    }

    current = current->forward[0];

    if (current == nullptr || current->key != key) {
        entry_count_++;
        total_size_bytes_ += (key.size() + value.size());
        int newLevel = randomLevel();

        // 如果新节点层数高于当前层数，更新上层指针
        if (newLevel > currentLevel_) {
            for (int i = currentLevel_; i < newLevel; i++) {
                update[i] = header_;
            }
            currentLevel_ = newLevel;
        }

        // 创建新节点
        Node *newNode = new Node(key, value, newLevel);

        // 更新各层指针
        for (int i = 0; i < newLevel; i++) {
            newNode->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = newNode;
        }
        // std::cout << "Inserted key " << key << " at level " << newLevel << std::endl;
        return true;
    } else {  // 如果key已存在，cover the old value
        total_size_bytes_ -= current->value.size();
        total_size_bytes_ += value.size();
        current->value = value;
        return true;
    }
}

// 查找键值
std::optional<std::string> SkipList::search(std::string key) {
    std::shared_lock<std::shared_mutex> memtable_lock(rw_mtx_);

    Node *current = header_;
    std::optional<std::string> ret;

    // 从最高层开始查找，直到底层（不从底层开始，前面跳跃，可以少查一些）
    for (int i = currentLevel_ - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr && current->forward[i]->key < key) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];
    // return current != nullptr && current->key == key;
    // return ((current != nullptr && current->key == key) ? current->value : "_N_E_K_");
    return ((current != nullptr && current->key == key) ? current->value : ret);
}

// 删除键值
bool SkipList::erase(std::string key) {
    std::unique_lock<std::shared_mutex> memtable_lock(rw_mtx_);

    std::vector<Node *> update(maxLevel_, nullptr);
    Node *current = header_;

    // 查找要删除的节点
    for (int i = currentLevel_ - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr && current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    // 如果找到key，则删除
    if (current != nullptr && current->key == key) {
        // 更新各层指针
        for (int i = 0; i < currentLevel_; i++) {
            if (update[i]->forward[i] != current) break;
            update[i]->forward[i] = current->forward[i];
        }

        delete current;

        // 如果删除的是最高层节点，降低currentLevel
        while (currentLevel_ > 1 && header_->forward[currentLevel_ - 1] == nullptr) {
            currentLevel_--;
        }

        // std::cout << "Deleted key " << key << std::endl;
        return true;
    } else {
        return false;  // key not exists
    }
}

// 打印跳表结构
void SkipList::display() {
    std::shared_lock<std::shared_mutex> memtable_lock(rw_mtx_);

    std::cout << "\n*****Skip List*****" << std::endl;
    for (int i = 0; i < currentLevel_; i++) {
        Node *node = header_->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != nullptr) {
            std::cout << node->key << ":" << node->value << " ";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

size_t SkipList::size() const { return total_size_bytes_; }

bool SkipList::empty() const { return entry_count_ == 0; }

std::vector<std::pair<std::string, std::string>> SkipList::traverse() {
    std::shared_lock<std::shared_mutex> memtable_lock(rw_mtx_);

    std::vector<std::pair<std::string, std::string>> result;

    Node *current = header_->forward[0];  // Level 0 is the lowest level (fully linked)
    while (current != nullptr) {
        result.emplace_back(current->key, current->value);
        current = current->forward[0];
    }

    return result;
}

size_t SkipList::GetMemTableSize() { return this->size(); }

}  // namespace delta
