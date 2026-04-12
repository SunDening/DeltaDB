#pragma once

#include <cstddef>
#include <cstdint>

#include <deltadb/utils/coding.h>
#include <deltadb/utils/comparator.h>
#include <deltadb/utils/config.h>
#include <deltadb/utils/filter_policy.h>
#include <deltadb/utils/log.h>

namespace delta {

extern delta::Config::ptr gDBConfig;

/**
 * ڲInternal Keyϵͳĺģ
 * һ⣺ڲ޸ûݵ£֧ MVCC汾ơɾǺͿո롣
 * ðװWrapper+ Adapterģʽ
 */

// òд gDBConfig

// ڲ룺| User Key (䳤) | 8-byte Tag (56λк + 8λ) |

// к
typedef uint64_t SequenceNumber;

static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

// 8 λ
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };

static const ValueType kValueTypeForSeek = kTypeValue;

struct ParsedInternalKey {
    std::string_view user_key;
    SequenceNumber sequence = 0;  // к - 56λ
    ValueType type = kTypeValue;  //  - 8λ

    ParsedInternalKey() = default;
    ParsedInternalKey(const std::string_view& u, const SequenceNumber& seq, ValueType t)
        : user_key(u), sequence(seq), type(t) {}
    std::string DebugString() const;
};

// keyл()ӵ*result
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Դ internal_key н user_keysequence  type洢 *result С
inline bool ParseInternalKey(const std::string_view& internal_key, ParsedInternalKey* result) {
    const size_t n = internal_key.size();
    if (n < 8) return false;
    uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
    uint8_t c = num & 0xff;
    result->sequence = num >> 8;
    result->type = static_cast<ValueType>(c);
    result->user_key = std::string_view(internal_key.data(), n - 8);
    return (c <= static_cast<uint8_t>(kTypeValue));
}

//  key ı볤  user_key(䳤) + 8 ֽ tag(56λк + 8λ)
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
    return key.user_key.size() + 8;  // User Key + 8-byte Tag
}

//  internal_key е user_key ֣ȥ 8 ֽ tag Ĳ֣
inline std::string_view ExtractUserKey(const std::string_view& internal_key) {
    assert(internal_key.size() >= 8);
    return std::string_view(internal_key.data(), internal_key.size() - 8);
}

class InternalKey {
   private:
    std::string rep_;  // ڲַ user_key  tagк + ͣ

   public:
    InternalKey() = default;
    InternalKey(const std::string_view& user_key, SequenceNumber s, ValueType t) {
        AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
    }

    bool DecodeFrom(const std::string_view& s) {
        if (s.size() < 8) return false;  // ڲ 8 ֽڣtag
        rep_.assign(s.data(), s.size());
        return true;
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
 * ڲıȽ
 *  user key ʹָıȽͨк
 */
class InternalKeyComparator : public Comparator {
   private:
    const Comparator* user_comparator_;  // ûṩıȽ

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
 * ¡İװࣺ
 *  - ڲתΪûٴ/ѯ
 *  - Ϊ¡ֻҪû
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
 * Ż.  DBImpl::Get() йҼ
+---------+-----------+--------+
| klength | user_key  |  tag   |
| varint32| char[k]   | uint64 |
+---------+-----------+--------+
                           
start_    kstart_            end_

 * Żɣ
    - ջ 200 ֽڻdb/dbformat.h#L215̼Ķѷ
    - ͼmemtable_key()ǰ׺ SkipList internal_key()ȣ SSTable
 */
class LookupKey {
   private:
    const char* start_;
    const char* kstart_;
    const char* end_;
    char space_[200];

   public:
    // ʼھָкŵĿϲ user_key
    LookupKey(const std::string_view& user_key, SequenceNumber sequence);

    LookupKey(const LookupKey&) = delete;
    LookupKey& operator=(const LookupKey&) = delete;

    ~LookupKey();

    //  MemTable Ҽ
    std::string_view memtable_key() const { return std::string_view(start_, end_ - start_); }

    // ڲ
    std::string_view internal_key() const { return std::string_view(kstart_, end_ - kstart_); }

    // û
    std::string_view user_key() const { return std::string_view(kstart_, end_ - kstart_ - 8); }
};

inline LookupKey::~LookupKey() {
    if (start_ != space_) delete[] start_;
}

}  // namespace delta