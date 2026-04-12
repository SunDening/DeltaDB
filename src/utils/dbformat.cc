#include <deltadb/utils/dbformat.h>

namespace delta {

/**
 * 将序列号和值类型打包成一个64位整数
 * Tag 结构：高位:序列号56位，低位:类型8位
 */
static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
    assert(seq <= kMaxSequenceNumber);
    assert(t <= kValueTypeForSeek);
    return (seq << 8) | t;  // 序列号左移8位，类型占低8位
}

// 将“key”的序列化(或编码后)附加到*result。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
    result->append(key.user_key.data(), key.user_key.size());
    PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

std::string ParsedInternalKey::DebugString() const { return "没有实现该方法"; }

std::string InternalKey::DebugString() const { return "没有实现该方法"; }

const char* InternalKeyComparator::Name() const { return "delta.InternalKeyComparator"; }

/**
 * 内部键比较规则：
 *  1. 首先按用户键升序比较（使用用户提供的比较器）
 *  2. 如果用户键相同，按序列号降序比较（新数据排在前面）
 *  3. 如果序列号也相同，按类型降序排列
 * 确保查找时能先看到最新版本的数据
 */
int InternalKeyComparator::Compare(const std::string_view& akey, const std::string_view& bkey) const {
    // 防御性检查：确保内部键至少包含 8 字节的 tag
    if (akey.size() < 8) {
        return -1;  // 无效键视为较小
    }
    if (bkey.size() < 8) {
        return +1;  // 无效键视为较大
    }

    int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
    if (r == 0) {
        // 用户键相同
        const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
        const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
        if (anum > bnum) {
            r = -1;
        } else if (anum < bnum) {
            r = +1;
        }
    }
    return r;
}

/**
 * 生成两个键之间的最短分隔符（用于 SST 文件的索引块）
 *  - 尝试缩短用户键部分
 *  - 附加最大序列号和 kValueTypeForSeek，确保分隔符在逻辑上仍然有效
 */
void InternalKeyComparator::FindShortestSeparator(std::string* start, const std::string_view& limit) const {
    std::string_view user_start = ExtractUserKey(*start);
    std::string_view user_limit = ExtractUserKey(limit);
    std::string tmp(user_start.data(), user_start.size());
    user_comparator_->FindShortestSeparator(&tmp, user_limit);
    if (tmp.size() < user_start.size() && user_comparator_->Compare(user_start, tmp) < 0) {
        PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
        assert(this->Compare(*start, tmp) < 0);
        assert(this->Compare(tmp, limit) < 0);
        start->swap(tmp);
    }
}

/**
 * 生成比给定键大的最短键（用于 SST 文件的索引块）
 *  - 尝试缩短用户键
 *  - 附加最大序列号
 */
void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
    std::string_view user_key = ExtractUserKey(*key);
    std::string tmp(user_key.data(), user_key.size());
    user_comparator_->FindShortSuccessor(&tmp);
    if (tmp.size() < user_key.size() && user_comparator_->Compare(user_key, tmp) < 0) {
        // User key has become shorter physically, but larger logically.
        // Tack on the earliest possible number to the shortened user key.
        PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
        assert(this->Compare(*key, tmp) < 0);
        key->swap(tmp);
    }
}

const char* InternalFilterPolicy::Name() const { return user_policy_->Name(); }

/**
 * 使用 keys 中的 user_key 构建过滤器（布隆过滤器）
 */
void InternalFilterPolicy::CreateFilter(const std::string_view* keys, int n, std::string* dst) const {
    std::string_view* mkeys = const_cast<std::string_view*>(keys);
    for (int i = 0; i < n; i++) {
        mkeys[i] = ExtractUserKey(keys[i]);
    }
    user_policy_->CreateFilter(keys, n, dst);
}

bool InternalFilterPolicy::KeyMayMatch(const std::string_view& key, const std::string_view& filter) const {
    return user_policy_->KeyMayMatch(ExtractUserKey(key), filter);
}

LookupKey::LookupKey(const std::string_view& user_key, SequenceNumber s) {
    size_t usize = user_key.size();
    size_t needed = usize + 13;
    char* dst;
    if (needed <= sizeof(space_)) {
        dst = space_;
    } else {
        dst = new char[needed];
    }
    start_ = dst;
    dst = EncodeVarint32(dst, usize + 8);      // 编码内部键空间
    kstart_ = dst;                             // 内部键起始指针
    std::memcpy(dst, user_key.data(), usize);  // 赋值内部键空间的 user_key 部分
    dst += usize;
    EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));  // tag 部分
    dst += 8;
    end_ = dst;
}

}  // namespace delta