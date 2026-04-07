#pragma once

#include "arena.h"
#include "log.h"
#include "util.h"

namespace delta {

/**
 * 该跳表实现要求写操作需外部同步（如互斥锁），而读操作只要保证跳表不被销毁即可无锁并发进行，
 * 其核心在于节点一旦插入便永不删除且数据不可变，仅通过安全的指针发布来保证线程安全。
 * 在并发访问方面，它做出了明确的划分：
 *  1. 写操作：必须由外部机制（如互斥锁 Mutex）进行同步，以防止并发写入导致的数据竞争；
 *  2.
 * 读操作：只要保证跳表在读取过程中不被销毁，就可以无锁并发进行，因为节点一旦插入后就不会被修改或删除，确保了数据的稳定性和一致性。
 * 为了支撑这种高效的读写模型，代码强制维护了以下两个关键不变性：
 * -
 * 内存生命周期不变性：一旦节点被分配，它将永远不会被删除，直到整个跳表被销毁。代码中完全不包含删除节点的逻辑，这从根本上避免了读线程访问到已被释放内存的问题。
 * -
 * 数据内容不变性：节点一旦被链接进跳表，其内容（除了用于构建链表结构的前后指针外）即变为不可变（Immutable）。所有的修改权发生在
 * Insert()
 * 操作中，且插入过程会先完整初始化节点，再利用“释放存储（release-stores）”等原子操作安全地将节点发布到一个或多个层级的链表中，从而保证了并发读取时数据的一致性。
 */

template <typename Key, class Comparator>
class SkipList {
   private:
    struct Node;

   public:
    /**
     * 功能定义：跳表对象将使用指定的 cmp 函数来比较键（keys），并使用 *arena 进行内存分配
     * 内存管理约束：通过 arena 分配的所有对象，其内存必须保持有效（即不能被释放），直到该跳表对象本身结束其生命周期
     */
    explicit SkipList(Comparator cmp, Arena* arena);

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 插入操作。要求表中不能存在与待插入键相等的元素。
    void Insert(const Key& key);

    // 查找操作。如果列表中存在与key相等的项，则返回true
    bool Contains(const Key& key) const;

    // 跳表内容迭代器
    class Iterator {
       public:
        explicit Iterator(const SkipList* list);

        // 如果迭代器位于有效节点，则返回true。
        bool Valid() const;

        // 返回当前位置的键。要求 Valid() 返回 true。
        const Key& key() const;

        // 将迭代器移动到下一个节点。要求 Valid() 返回 true。
        void Next();

        // 将迭代器移动到上一个节点。要求 Valid() 返回 true。
        void Prev();

        // 将迭代器移动到 key >= target 的第一个节点。
        void Seek(const Key& target);

        // 将迭代器移动到第一个节点。如果list不为空，迭代器的最终状态为Valid()
        void SeekToFirst();

        // 将迭代器移动到最后一个节点。如果list不为空，迭代器的最终状态为Valid()
        void SeekToLast();

       private:
        const SkipList* list_;
        Node* node_;
    };

   private:
    enum { kMaxHeight = 12 };  // 跳表的最大高度

    inline int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

    Node* NewNode(const Key& key, int height);
    int RandomHeight();
    bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

    // 如果key大于存储在“n”中的数据，则返回true
    bool KeyIsAfterNode(const Key& key, Node* n) const;

    // 返回键处或键后最早的节点。如果没有，返回nullptr
    Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

    // 返回键前最早的节点。如果没有，返回 head_
    Node* FindLessThan(const Key& key) const;

    // 返回最后一个节点。如果没有，返回 head_
    Node* FindLast() const;

    Comparator const compare_;  // 比较器

    Arena* const arena_;  // 内存分配器

    Node* const head_;  // 跳表的头节点（哨兵）

    std::atomic<int> max_height_;  // 跳表的当前最大高度

    Random rnd_;  // 用于生成随机高度的随机数生成器
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
    explicit Node(const Key& k) : key(k) {}

    Key const key;  // 节点的键

    // 获取在第 n 层的下一个节点（带内存屏障）
    Node* Next(int n) {
        assert(n >= 0);
        return next_[n].load(std::memory_order_acquire);
    }

    // 设置在第 n 层的下一个节点（带内存屏障）
    void SetNext(int n, Node* x) {
        assert(n >= 0);
        next_[n].store(x, std::memory_order_release);
    }

    // 获取在第 n 层的下一个节点（不带内存屏障）
    Node* NoBarrier_Next(int n) {
        assert(n >= 0);
        return next_[n].load(std::memory_order_relaxed);
    }

    void NoBarrier_SetNext(int n, Node* x) {
        assert(n >= 0);
        next_[n].store(x, std::memory_order_relaxed);
    }

   private:
    std::atomic<Node*> next_[1];  // 指向下一节点的指针数组，实际大小由节点的高度决定
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key, int height) {
    // 计算需要的内存大小
    // sizeof(Node) + 每层指针的大小 * (height - 1)
    char* const node_memory = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    // Placement new 在分配的内存上构造 Node
    return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
    list_ = list;
    node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
    return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
    assert(Valid());
    return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
    assert(Valid());
    node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
    assert(Valid());
    node_ = list_->FindLessThan(node_->key);
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) {
        node_ = nullptr;
    }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
    static const unsigned int kBranching = 4;  // 每层的分支因子
    int height = 1;
    while (height < kMaxHeight && ((rnd_.Next() % kBranching) == 0)) {
        height++;
    }
    assert(height > 0);
    assert(height <= kMaxHeight);
    return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_(n->key, key) < 0);
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                                                                        Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (KeyIsAfterNode(key, next)) {
            x = next;
        } else {
            if (prev != nullptr) prev[level] = x;
            if (level == 0) {
                return next;
            } else {
                level--;
            }
        }
    }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        assert(x == head_ || compare_(x->key, key) < 0);
        Node* next = x->Next(level);
        if (next == nullptr || compare_(next->key, key) >= 0) {
            if (level == 0) {
                return x;
            } else {
                // Switch to next list
                level--;
            }
        } else {
            x = next;
        }
    }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
        Node* next = x->Next(level);
        if (next == nullptr) {
            if (level == 0) {
                return x;
            } else {
                // Switch to next list
                level--;
            }
        } else {
            x = next;
        }
    }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode("", kMaxHeight)), max_height_(1), rnd_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; i++) {
        head_->SetNext(i, nullptr);
    }
    InfoLog << "SkipList created with max height " << kMaxHeight;
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    // 我们的数据结构不允许重复插
    assert(x == nullptr || !Equal(key, x->key));

    int height = RandomHeight();
    if (height > GetMaxHeight()) {
        for (int i = GetMaxHeight(); i < height; i++) {
            prev[i] = head_;
        }
        max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; i++) {
        x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
        prev[i]->SetNext(i, x);
    }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    if (x != nullptr && Equal(key, x->key)) {
        return true;
    } else {
        return false;
    }
}

}  // namespace delta