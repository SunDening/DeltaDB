#include <deltadb/table/iter_merger.h>
#include <deltadb/table/iterator_wrapper.h>
#include <deltadb/utils/comparator.h>
#include <deltadb/utils/iterator.h>

namespace delta {

/**
 * @brief 将多个有序子迭代器合并为一个迭代器
 * 核心特性：
 *  - 按 key 顺序输出（由 Comparator 决定大小）
 *  - 不做去重：同一个 key 在 K 个子迭代器中存在时会输出 K 次
 *  - 支持双向遍历：正向 (Next) 和反向 (Prev)
 */
class MergeIterator : public Iterator {
   private:
    // 迭代方向枚举
    enum Direction { kForward, kReverse };

    const Comparator* comparator_;  // 比较器
    IteratorWrapper* children_;     // 子迭代器的包装类
    int n_;                         // 子迭代器数量
    IteratorWrapper* current_;      // 指向当前最小的迭代器
    Direction direction_;           // 方向，支持正向（Next()）和反向（Prev()）遍历，切换方向时会重新定位所有子迭代器

    /**
     * @brief 线性扫描所有子迭代器，找出 key 最小的
     */
    void FindSmallest();

    /**
     * @brief 线性扫描所有子迭代器，找出 key 最大的
     */
    void FindLargest();

   public:
    /**
     * @param comparator：比较器
     * @param children：子迭代器数组
     * @param n：子迭代器数量
     */
    MergeIterator(const Comparator* comparator, Iterator** children, int n)
        : comparator_(comparator), children_(new IteratorWrapper[n]), n_(n), current_(nullptr), direction_(kForward) {
        for (int i = 0; i < n; i++) {
            children_[i].Set(children[i]);
        }
    }

    ~MergeIterator() override { delete[] children_; }

    bool Valid() const override { return (current_ != nullptr); }

    /**
     * @brief 定位到全局最小的 key
     */
    void SeekToFirst() override {
        // 所有子迭代器各自定位到起始位置
        for (int i = 0; i < n_; i++) {
            children_[i].SeekToFirst();
        }
        // 找出所有子迭代器中 key 最小的作为 current_
        FindSmallest();
        // 标记为正向遍历
        direction_ = kForward;
    }

    /**
     * @brief 定位到全局最大的 key
     */
    void SeekToLast() override {
        // 所有子迭代器各自定位到末尾位置
        for (int i = 0; i < n_; i++) {
            children_[i].SeekToLast();
        }
        // 找出所有子迭代器中 key 最大的作为 current_
        FindLargest();
        // 标记为反向遍历
        direction_ = kReverse;
    }

    /**
     * @brief  定位到 >= target 的第一个 key
     */
    void Seek(const std::string_view& target) override {
        // 所有子迭代器各自 Seek 到 >= target的位置
        for (int i = 0; i < n_; i++) {
            children_[i].Seek(target);
        }
        // 找出最小的子迭代器作为 current_
        FindSmallest();
        direction_ = kForward;
    }

    /**
     * @brief 正向遍历
     */
    void Next() override {
        assert(Valid());
        // 如果之前是反向遍历，需要重新定位所有非 current_ 的子迭代器
        // 因为反向遍历时，其他子迭代器可能位于当前 key 之前
        if (direction_ != kForward) {
            for (int i = 0; i < n_; i++) {
                IteratorWrapper* child = &children_[i];
                if (child != current_) {
                    // 定位到 >= 当前 key 的位置
                    child->Seek(key());
                    // 如果正好等于当前 key，再 Next() 跳过，确保 > 当前 key
                    if (child->Valid() && comparator_->Compare(key(), child->key()) == 0) {
                        child->Next();
                    }
                }
            }
            direction_ = kForward;
        }

        // current_ 前进到下一个位置
        current_->Next();

        // 重新找出最小的子迭代器
        FindSmallest();
    }

    /**
     * @brief 上一个位置
     */
    void Prev() override {
        assert(Valid());

        // 如果之前是正向遍历，需要重新定位所有非 current_ 的子迭代器
        // 正向遍历时，其他子迭代器可能位于 key 之后
        if (direction_ != kReverse) {
            for (int i = 0; i < n_; i++) {
                IteratorWrapper* child = &children_[i];
                if (child != current_) {
                    child->Seek(key());
                    if (child->Valid()) {
                        // 子迭代器在第一个 >= key() 的位置，Prev() 回退到 < key()
                        child->Prev();
                    } else {
                        // 该子迭代器所有 entry 都 < key()，直接定位到最后一个
                        child->SeekToLast();
                    }
                }
            }
            direction_ = kReverse;
        }

        // current_ 回退到上一个位置
        current_->Prev();

        // 重新找出最大的子迭代器
        FindLargest();
    }

    std::string_view key() const override {
        assert(Valid());
        return current_->key();
    }

    std::string_view value() const override {
        assert(Valid());
        return current_->value();
    }

    Status status() const override {
        Status status;
        for (int i = 0; i < n_; i++) {
            status = children_[i].status();
            if (!status.ok()) {
                break;
            }
        }
        return status;
    }
};

/**
 * @brief 线性扫描所有子迭代器，找出 key 最小的迭代器
 */
void MergeIterator::FindSmallest() {
    IteratorWrapper* smallest = nullptr;
    for (int i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child->Valid()) {
            if (smallest == nullptr) {
                smallest = child;
            } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
                smallest = child;
            }
        }
    }
    current_ = smallest;
}

void MergeIterator::FindLargest() {
    IteratorWrapper* largest = nullptr;
    for (int i = n_ - 1; i >= 0; i--) {
        IteratorWrapper* child = &children_[i];
        if (child->Valid()) {
            if (largest == nullptr) {
                largest = child;
            } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
                largest = child;
            }
        }
    }
    current_ = largest;
}

/**
 * @brief 创建 MergeIterator 的工厂函数
 */
Iterator* NewMergeIterator(const Comparator* comparator, Iterator** children, int n) {
    assert(n >= 0);
    if (n == 0) {
        // 没有子迭代器：返回空迭代器
        return NewEmptyIterator();
    } else if (n == 1) {
        // 只有一个子迭代器：直接返回，避免额外开销
        return children[0];
    } else {
        // 多个子迭代器：创建 MergingIterator
        return new MergeIterator(comparator, children, n);
    }
}

}  // namespace delta