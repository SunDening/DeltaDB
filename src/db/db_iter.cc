#include <deltadb/db/db_impl.h>
#include <deltadb/db/db_iter.h>
#include <deltadb/db/filename.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/utils/iterator.h>

namespace delta {

namespace {

/**
 * @brief DBIter 类，用户迭代器的核心实现。
 * 负责：
 *  1. MVCC 版本过滤：只返回指定序列号可见的数据
 *  2. 键格式转换：将内部键转换为用户键
 *  3. 删除标记处理：隐藏被删除的键
 *  4. 多版本合并：同一键的多个版本合并为单个可见版本
 */
class DBIter : public Iterator {
   public:
    enum Direction { kForward, kReverse };

   private:
    DBImpl* db_;                               // 数据库实例，用于记录读取采样
    const Comparator* const user_comparator_;  // 用户键比较器
    Iterator* const iter_;                     // 底层内部迭代器（遍历所有版本）
    SequenceNumber const sequence_;            // 快照序列号，决定可见性
    Status status_;
    std::string saved_key_;    // 反向迭代时保存当前键
    std::string saved_value_;  // 反向迭代时保存当前值
    Direction direction_;      // 当前迭代方向
    bool valid_;               // 迭代器是否有效
    Random rnd_;
    size_t bytes_until_read_sampling_;  // 距离下次读取采样的字节数

    /**
     * @brief 查找下一个有效用户条目（正向）
     */
    void FindNextUserEntry(bool skipping, std::string* skip);

    /**
     * @brief 查找上一个有效用户条目（反向）
     */
    void FindPrevUserEntry();

    /**
     * @brief 解析内部键并记录读取采样
     */
    bool ParseKey(ParsedInternalKey* key);

    inline void SaveKey(const std::string_view& k, std::string* dst) { dst->assign(k.data(), k.size()); }

    inline void ClearSavedValue() {
        if (saved_value_.capacity() > 1048576) {
            std::string empty;
            swap(empty, saved_value_);
        } else {
            saved_value_.clear();
        }
    }

    size_t RandomCompactionPeriod() { return rnd_.Uniform(2 * gDBConfig->read_bytes_period); }

   public:
    DBIter(DBImpl* db, const Comparator* comp, Iterator* iter, SequenceNumber s, uint32_t seed)
        : db_(db),
          user_comparator_(comp),
          iter_(iter),
          sequence_(s),
          direction_(kForward),
          valid_(false),
          rnd_(seed),
          bytes_until_read_sampling_(RandomCompactionPeriod()) {}

    DBIter(const DBIter&) = delete;
    DBIter& operator=(const DBIter&) = delete;

    ~DBIter() override { delete iter_; }

    bool Valid() const override { return valid_; }

    std::string_view key() const override {
        assert(valid_);
        return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
    }

    std::string_view value() const override {
        assert(valid_);
        return (direction_ == kForward) ? iter_->value() : saved_value_;
    }

    Status status() const override {
        if (status_.ok()) {
            return iter_->status();
        } else {
            return status_;
        }
    }

    void Next() override;
    void Prev() override;
    void Seek(const std::string_view& target) override;
    void SeekToFirst() override;
    void SeekToLast() override;
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
    std::string_view k = iter_->key();

    // 读取采样逻辑（用于触发自动压缩）
    size_t bytes_read = k.size() + iter_->value().size();
    while (bytes_until_read_sampling_ < bytes_read) {
        bytes_until_read_sampling_ += RandomCompactionPeriod();
        db_->RecordReadSample(k);  // 记录读取样本，可能触发压缩
    }
    assert(bytes_until_read_sampling_ >= bytes_read);
    bytes_until_read_sampling_ -= bytes_read;

    // 解析内部键：user_key + sequence + type
    if (!ParseInternalKey(k, ikey)) {
        status_ = Status::Corruption("corrupted internal key in DBIter");
        return false;
    } else {
        return true;
    }
}

void DBIter::Next() {
    assert(valid_);

    // 方向切换处理
    if (direction_ == kReverse) {
        direction_ = kForward;  // 从反向切换到正向
        if (!iter_->Valid()) {
            iter_->SeekToFirst();
        } else {
            iter_->Next();
        }
        if (!iter_->Valid()) {
            valid_ = false;
            saved_key_.clear();
            return;
        }
    } else {
        // 保存当前键，用于后续跳过
        SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

        // 直接移动到下一个，避免重复检查当前键
        iter_->Next();
        if (!iter_->Valid()) {
            valid_ = false;
            saved_key_.clear();
            return;
        }
    }

    // 查找下一个有效的用户条目
    FindNextUserEntry(true, &saved_key_);
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
    assert(iter_->Valid());
    assert(direction_ == kForward);

    // 循环直到找到一个可接受的条目
    do {
        ParsedInternalKey ikey;
        // 解析内部键，并检查序列号是否可见
        if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
            switch (ikey.type) {
                case kTypeDeletion:
                    SaveKey(ikey.user_key, skip);
                    skipping = true;
                    break;
                case kTypeValue:
                    if (skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
                    } else {
                        valid_ = true;
                        saved_key_.clear();
                        return;
                    }
                    break;
            }
        }
        iter_->Next();
    } while (iter_->Valid());

    // 没有更多有效条目
    saved_key_.clear();
    valid_ = false;
}

void DBIter::Prev() {
    assert(valid_);

    // 方向切换处理
    if (direction_ == kForward) {
        assert(iter_->Valid());
        // 需要向后扫描直到键改变，然后使用正常的反向扫描逻辑
        SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
        while (true) {
            iter_->Prev();
            if (!iter_->Valid()) {
                valid_ = false;
                saved_key_.clear();
                ClearSavedValue();
                return;
            }
            // 直到找到一个用户键更小的条目
            if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) < 0) {
                break;
            }
        }
        direction_ = kReverse;
    }
    FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
    assert(direction_ == kReverse);

    ValueType value_type = kTypeDeletion;

    if (iter_->Valid()) {
        do {
            ParsedInternalKey ikey;
            // 解析并检查序列号可见性
            if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
                // 如果已经找到非删除值，且当前键更小，则停止
                if ((value_type != kTypeDeletion) && user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
                    break;
                }
                value_type = ikey.type;
                if (value_type == kTypeDeletion) {
                    // 删除标记：清空保存的键和值
                    saved_key_.clear();
                    ClearSavedValue();
                } else {
                    // 普通值：保存键和值
                    std::string_view raw_value = iter_->value();
                    // 内存优化：如果saved_value_ 容量过大则释放
                    if (saved_value_.capacity() > raw_value.size() + 1048576) {
                        std::string empty;
                        swap(empty, saved_value_);
                    }
                    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
                    saved_value_.assign(raw_value.data(), raw_value.size());
                }
            }
            iter_->Prev();
        } while (iter_->Valid());
    }

    // 根据最后遇到的类型决定是否有效
    if (value_type == kTypeDeletion) {
        // 最后遇到的是删除标记，该键对用户不可见
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        direction_ = kForward;
    } else {
        valid_ = true;
    }
}

void DBIter::Seek(const std::string_view& target) {
    direction_ = kForward;
    ClearSavedValue();
    saved_key_.clear();

    // 构造内部键进行查找
    // 使用 kValueTypeForSeek 确保定位到正确的范围
    AppendInternalKey(&saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
    iter_->Seek(saved_key_);
    if (iter_->Valid()) {
        FindNextUserEntry(false, &saved_key_);
    } else {
        valid_ = false;
    }
}

void DBIter::SeekToFirst() {
    direction_ = kForward;
    ClearSavedValue();
    iter_->SeekToFirst();
    if (iter_->Valid()) {
        FindNextUserEntry(false, &saved_key_);
    } else {
        valid_ = false;
    }
}

void DBIter::SeekToLast() {
    direction_ = kReverse;
    ClearSavedValue();
    iter_->SeekToLast();
    FindPrevUserEntry();
}

}  // namespace

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed) {
    return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace delta