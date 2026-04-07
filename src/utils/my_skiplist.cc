#include "my_skiplist.h"

namespace delta {

SkipList::SkipList(int maxLvl, float p) : maxLevel_(maxLvl), probability_(p), currentLevel_(1) {
    this->header_ = new Node("", "", maxLevel_);
    entry_count_ = 0;
    total_size_bytes_ = 0;
}

SkipList::~SkipList() {
    Node *current = header_->forward[0];
    while (current != nullptr) {
        Node *next = current->forward[0];
        delete current;
        current = next;
    }
    delete header_;
}

int SkipList::randomLevel() {
    int lvl = 1;
    while ((rand() % 100) < (probability_ * 100) && lvl < maxLevel_) {
        lvl++;
    }
    return lvl;
}

// 插入键值
bool SkipList::insert(const std::string &key, const std::string &value) {
    std::vector<Node *> update(maxLevel_, nullptr);
    Node *current = header_;

    std::unique_lock<std::shared_mutex> write_lock(rw_mtx_);

    // 从最高层开始查找插入位置
    for (int i = currentLevel_ - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr && current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;  // 记录每层需要更新的节点
    }

    current = current->forward[0];

    if (current == nullptr || current->key != key) {
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

        entry_count_++;
        total_size_bytes_ += (key.size() + value.size());
        // 更新各层指针
        for (int i = 0; i < newLevel; i++) {
            newNode->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = newNode;
        }

        InfoLog << "insert new node success! key: " << key << ", value:" << value;
    } else {  // 如果key已存在，cover the old value
        total_size_bytes_ -= current->value.size();
        total_size_bytes_ += value.size();
        current->value = value;
        InfoLog << "key: " << key << " already exists, new value: " << value << " covers the old value";
    }
    return true;
}

// 查找键值
std::optional<std::string> SkipList::search(const std::string &key) {
    Node *current = header_;
    std::optional<std::string> ret;

    {
        std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);
        // 从最高层开始查找，直到底层（不从底层开始，前面跳跃，可以少查一些）
        for (int i = currentLevel_ - 1; i >= 0; i--) {
            while (current->forward[i] != nullptr && current->forward[i]->key < key) {
                current = current->forward[i];
            }
        }

        current = current->forward[0];
    }
    InfoLog << "search key: " << key;
    return ((current != nullptr && current->key == key) ? current->value : ret);
}

// 删除键值
bool SkipList::erase(const std::string &key) {
    std::vector<Node *> update(maxLevel_, nullptr);
    Node *current = header_;
    {
        std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);
        // 查找要删除的节点
        for (int i = currentLevel_ - 1; i >= 0; i--) {
            while (current->forward[i] != nullptr && current->forward[i]->key < key) {
                current = current->forward[i];
            }
            update[i] = current;
        }

        current = current->forward[0];
    }
    bool find;
    {
        std::unique_lock<std::shared_mutex> write_lock(rw_mtx_);
        // 如果找到key，则删除
        find = current != nullptr && current->key == key;
        if (find) {
            // 更新各层指针
            for (int i = 0; i < currentLevel_; i++) {
                if (update[i]->forward[i] != current) break;
                update[i]->forward[i] = current->forward[i];
            }
            total_size_bytes_ -= current->key.size();
            total_size_bytes_ -= current->value.size();
            entry_count_--;
            delete current;

            // 如果删除的是最高层节点，降低currentLevel
            while (currentLevel_ > 1 && header_->forward[currentLevel_ - 1] == nullptr) {
                currentLevel_--;
            }
        }
    }
    InfoLog << "erase key: " << key << (find ? " success." : " failed, not find the key.");
    return find;
}

// 打印跳表结构
void SkipList::display() {
    std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);

    std::cout << "\n*****Skip List*****\n";
    for (int i = 0; i < currentLevel_; i++) {
        Node *node = header_->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != nullptr) {
            std::cout << node->key << ":" << node->value << " ";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;
}

size_t SkipList::size() {
    std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);
    return total_size_bytes_;
}

size_t SkipList::count() {
    std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);
    return entry_count_;
}

bool SkipList::empty() {
    std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);
    return entry_count_ == 0;
}

std::vector<std::pair<std::string, std::string>> SkipList::traverse() {
    InfoLog << "traverse skiplist";
    std::shared_lock<std::shared_mutex> read_lock(rw_mtx_);

    std::vector<std::pair<std::string, std::string>> result;

    Node *current = header_->forward[0];  // Level 0 is the lowest level (fully linked)
    while (current != nullptr) {
        result.emplace_back(current->key, current->value);
        current = current->forward[0];
    }

    return result;
}
}  // namespace delta