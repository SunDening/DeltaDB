#include "memtable.h"

using namespace std;


int MemTable::randomLevel() {
    int lvl = 1;
    while ((rand() % 100) < (probability * 100) && lvl < maxLevel) {
        lvl++;
    }
    return lvl;
}

// 插入键值
bool MemTable::insert(string key, string value) {

    unique_lock<shared_mutex> memtable_lock(memtable_rwmutex);

    vector<Node *> update(maxLevel, nullptr);
    Node *current = header;

    // 从最高层开始查找插入位置
    for (int i = currentLevel - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr &&
               current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current; // 记录每层需要更新的节点
    }

    current = current->forward[0];

    if (current == nullptr || current->key != key) {
        entry_count++;
        total_size_bytes += (key.size() + value.size());
        int newLevel = randomLevel();

        // 如果新节点层数高于当前层数，更新上层指针
        if (newLevel > currentLevel) {
            for (int i = currentLevel; i < newLevel; i++)
            {
                update[i] = header;
            }
            currentLevel = newLevel;
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
        total_size_bytes -= current->value.size();
        total_size_bytes += value.size();
        current->value = value;
        return true;
    }
}

// 查找键值
optional<string> MemTable::search(string key) {

    shared_lock<shared_mutex> memtable_lock(memtable_rwmutex);

    Node *current = header;
    optional<string> ret;

    // 从最高层开始查找，直到底层（不从底层开始，前面跳跃，可以少查一些）
    for (int i = currentLevel - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr &&
               current->forward[i]->key < key) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];
    // return current != nullptr && current->key == key;
    // return ((current != nullptr && current->key == key) ? current->value : "_N_E_K_");
    return ((current != nullptr && current->key == key) ? current->value : ret);
}

// 删除键值
bool MemTable::erase(string key) {

    unique_lock<shared_mutex> memtable_lock(memtable_rwmutex);

    vector<Node *> update(maxLevel, nullptr);
    Node *current = header;

    // 查找要删除的节点
    for (int i = currentLevel - 1; i >= 0; i--) {
        while (current->forward[i] != nullptr &&
               current->forward[i]->key < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    // 如果找到key，则删除
    if (current != nullptr && current->key == key) {
        // 更新各层指针
        for (int i = 0; i < currentLevel; i++)
        {
            if (update[i]->forward[i] != current)
                break;
            update[i]->forward[i] = current->forward[i];
        }

        delete current;

        // 如果删除的是最高层节点，降低currentLevel
        while (currentLevel > 1 && header->forward[currentLevel - 1] == nullptr)
        {
            currentLevel--;
        }

        // std::cout << "Deleted key " << key << std::endl;
        return true;
    } else {
        return false;  // key not exists
    }
}

// 打印跳表结构
void MemTable::display() {

    shared_lock<shared_mutex> memtable_lock(memtable_rwmutex);

    std::cout << "\n*****Skip List*****" << std::endl;
    for (int i = 0; i < currentLevel; i++) {
        Node *node = header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != nullptr)
        {
            std::cout << node->key << ":" << node->value << " ";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

size_t MemTable::size() const {
    return total_size_bytes;
}

bool MemTable::empty() const {
    return entry_count == 0;
}

vector<pair<string, string>> MemTable::traverse() {

    shared_lock<shared_mutex> memtable_lock(memtable_rwmutex);

    vector<std::pair<std::string, std::string>> result;

    Node* current = header->forward[0];  // Level 0 is the lowest level (fully linked)
    while (current != nullptr) {
        result.emplace_back(current->key, current->value);
        current = current->forward[0];
    }

    return result;
}

size_t MemTable::GetMemTableSize() { return this->size(); }