#pragma once

#include <deltadb/utils/iterator.h>

namespace delta {

/**
 * IteratorWrapper 是 Iterator 的内部包装类，
 * 通过缓存 Valid() 和 Key() 的结果来减少虚函数调用，提高缓存局部性，从而提升迭代器性能。
 * 缓存 valid_ 和 key_ 到 IteratorWrapper 中
 * valid_ 和 key_ 是成员变量，访问无需虚函数，且有更好的缓存局部性
 */

class IteratorWrapper {
   private:
    Iterator* iter_;        // 底层迭代器（拥有所有权）
    bool valid_;            // 缓存的 Valid() 结果
    std::string_view key_;  // 缓存的 key() 结果

    /**
     * @brief 更新缓存状态
     * 每次底层迭代器状态改变后调用
     */
    void Update() {
        valid_ = iter_->Valid();  // 缓存 Valid() 结果
        if (valid_) {
            key_ = iter_->key();  // 缓存 key() 结果（Slice 是轻量级引用）
        }
    }

   public:
    IteratorWrapper() : iter_(nullptr), valid_(false) {}

    explicit IteratorWrapper(Iterator* iter) : iter_(nullptr) { Set(iter); }
    ~IteratorWrapper() { delete iter_; }
    Iterator* iter() const { return iter_; }

    /**
     * @brief 设置底层迭代器
     */
    void Set(Iterator* iter) {
        delete iter_;  // 删除旧迭代器
        iter_ = iter;
        if (iter_ == nullptr) {
            valid_ = false;
        } else {
            Update();
        }
    }

    /**
     * @brief 返回缓存的 valid_
     */
    bool Valid() const { return valid_; }

    /**
     * @brief 返回缓存的 key_（无虚函数调用）
     */
    std::string_view key() const {
        assert(Valid());
        return key_;
    }

    /**
     * @brief 转发到 iter_->value()
     */
    std::string_view value() const {
        assert(Valid());
        return iter_->value();
    }

    /**
     * @brief 转发到 iter_->status()
     */
    Status status() const {
        assert(iter_);
        return iter_->status();
    }

    /**
     * @brief 前进 + 更新缓存
     */
    void Next() {
        assert(iter_);
        iter_->Next();
        Update();
    }

    /**
     * 后退 + 更新缓存
     */
    void Prev() {
        assert(iter_);
        iter_->Prev();
        Update();
    }

    /**
     * @brief 定位 + 更新缓存
     */
    void Seek(const std::string_view& k) {
        assert(iter_);
        iter_->Seek(k);
        Update();
    }

    /**
     * @brief 定位到首 + 更新缓存
     */
    void SeekToFirst() {
        assert(iter_);
        iter_->SeekToFirst();
        Update();
    }

    /**
     * @brief 定位到尾 + 更新缓存
     */
    void SeekToLast() {
        assert(iter_);
        iter_->SeekToLast();
        Update();
    }
};

}  // namespace delta
