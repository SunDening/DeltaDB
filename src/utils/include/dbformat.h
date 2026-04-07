#pragma once

#include <cstddef>
#include <cstdint>

#include "coding.h"
#include "comparator.h"
#include "config.h"
#include "filter_policy.h"

namespace delta {

extern delta::Config::ptr gDBConfig;

/**
 * 是内部键（Internal Key）系统的核心，
 * 解决了一个根本问题：如何在不修改用户数据的情况下，支持 MVCC（多版本并发）控制、删除标记和快照隔离。
 * 采用包装器（Wrapper）+ 适配器（Adapter）模式
 */

// 配置参数都写到了 gDBConfig

// 内部键编码：| User Key (变长) | 8-byte Tag (56位序列号 + 8位类型) |

// 序列号
typedef uint64_t SequenceNumber;

static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

// 8 位类型
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };

static const ValueType kValueTypeForSeek = kTypeValue;

struct ParsedInternalKey {
    std::string_view user_key;
    SequenceNumber sequence = 0;  // 序列号 - 56位
    ValueType type = kTypeValue;  // 类型 - 8位

    ParsedInternalKey() = default;
    ParsedInternalKey(const std::string_view& u, const SequenceNumber& seq, ValueType t)
        : user_key(u), sequence(seq), type(t) {}
    std::string DebugString() const;
};

// 将“key”的序列化(或编码后)附加到*result。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// 尝试从 internal_key 中解析出 user_key、sequence 和 type，并将结果存储在 *result 中。
inline bool ParseInternalKey(const std::string_view& internal_key, ParsedInternalKey* result) {
    const size_t n = internal_key.size();
    if (n < 8) return false;
    uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
    uint8_t c = num & 0xff;
    result->sequence = num >> 8;
    result->type = static_cast<ValueType>(c);
    result->user_key = std::string(internal_key.data(), n - 8);
    return (c <= static_cast<uint8_t>(kTypeValue));
}

// 返回 key 的编码长度 —— user_key(变长) + 8 字节 tag(56位序列号 + 8位类型)
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
    return key.user_key.size() + 8;  // User Key + 8-byte Tag
}

// 返回 internal_key 中的 user_key 部分（即去掉最后 8 字节 tag 后的部分）。
inline std::string_view ExtractUserKey(const std::string_view& internal_key) {
    assert(internal_key.size() >= 8);
    return std::string_view(internal_key.data(), internal_key.size() - 8);
}

class InternalKey {
   private:
    std::string rep_;  // 编码后的内部键字符串，包含 user_key 和 tag（序列号 + 类型）

   public:
    InternalKey() = default;
    InternalKey(const std::string_view& user_key, SequenceNumber s, ValueType t) {
        AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
    }

    bool DecodeFrom(const std::string_view& s) {
        rep_.assign(s.data(), s.size());
        return !rep_.empty();
    }

    std::string_view Encode() const {
        assert(!rep_.empty());
        return rep_;
    }

    std::string_view user_key() const { return ExtractUserKey(rep_); }

    void SetFrom(const ParsedInternalKey& p) {
        rep_.clear();
        AppendInternalKey(&rep_, p);
    }

    void Clear() { rep_.clear(); }

    std::string DebugString() const;
};

/**
 * 内部键的比较器，
 * 它对 user key 部分使用指定的比较器，并通过减少序列号来打破连接
 */
class InternalKeyComparator : public Comparator {
   private:
    const Comparator* user_comparator_;  // 用户提供的比较器

   public:
    explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}

    const char* Name() const override;
    int Compare(const std::string_view& a, const std::string_view& b) const override;
    void FindShortestSeparator(std::string* start, const std::string_view& limit) const override;
    void FindShortSuccessor(std::string* key) const override;

    const Comparator* user_comparator() const { return user_comparator_; }

    int Compare(const InternalKey& a, const InternalKey& b) const;
};

inline int InternalKeyComparator::Compare(const InternalKey& a, const InternalKey& b) const {
    return Compare(a.Encode(), b.Encode());
}

/**
 * 布隆过滤器的包装类：
 *  - 将内部键转换为用户键后再创建/查询过滤器
 *  - 因为布隆过滤器只需要关心用户键
 */
class InternalFilterPolicy : public FilterPolicy {
   private:
    const FilterPolicy* const user_policy_;

   public:
    explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) {}

    const char* Name() const override;
    void CreateFilter(const std::string_view* keys, int n, std::string* dst) const override;
    bool KeyMayMatch(const std::string_view& key, const std::string_view& filter) const override;
};

/**
 * 查找优化. 用于 DBImpl::Get() 操作中构造查找键：
+---------+-----------+--------+
| klength | user_key  |  tag   |
| varint32| char[k]   | uint64 |
+---------+-----------+--------+
↑         ↑                  ↑
start_    kstart_            end_

 * 优化技巧：
    - 栈分配 200 字节缓冲区（db/dbformat.h#L215），避免短键的堆分配
    - 两层视图：memtable_key()（含长度前缀，用于 SkipList）和 internal_key()（不含长度，用于 SSTable）
 */
class LookupKey {
   private:
    const char* start_;
    const char* kstart_;
    const char* end_;
    char space_[200];

   public:
    // 初始化，用于在具有指定序列号的快照上查找 user_key
    LookupKey(const std::string_view& user_key, SequenceNumber sequence);

    LookupKey(const LookupKey&) = delete;
    LookupKey& operator=(const LookupKey&) = delete;

    ~LookupKey();

    // 返回完整的 MemTable 查找键
    std::string_view memtable_key() const { return std::string_view(start_, end_ - start_); }

    // 返回内部键部分
    std::string_view internal_key() const { return std::string_view(kstart_, end_ - kstart_); }

    // 返回用户键部分
    std::string_view user_key() const { return std::string_view(kstart_, end_ - kstart_ - 8); }
};

inline LookupKey::~LookupKey() {
    if (start_ != space_) delete[] start_;
}

}  // namespace delta