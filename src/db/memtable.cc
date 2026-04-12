#include <deltadb/db/memtable.h>

namespace delta {

/**
 * 从数据中解析 长度前缀切片 （varint32 长度 + 实际数据）
 * +5 是因为 varint32 最多 5 字节
 */
static std::string_view GetLengthPrefixedSlice(const char* data) {
    uint32_t len;
    const char* p = data;
    p = GetVarint32Ptr(p, p + 5, &len);
    return std::string_view(p, len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

int MemTable::KeyComparator::operator()(const char* aptr, const char* bptr) const {
    std::string_view a = GetLengthPrefixedSlice(aptr);
    std::string_view b = GetLengthPrefixedSlice(bptr);
    return comparator.Compare(a, b);
}

/**
 * 将用户 key 编码为跳表使用的 InternalKey 格式（长度前缀 + 数据）
 * 用于 Seek() 查找时的 key 编码
 */
static const char* EncodeKey(std::string* scratch, const std::string_view& target) {
    scratch->clear();
    PutVarint32(scratch, target.size());
    scratch->append(target.data(), target.size());
    return scratch->data();
}

class MemTableIterator : public Iterator {
   private:
    MemTable::Table::Iterator iter_;
    std::string tmp_;  // 用于 EncodeKey 的临时缓冲区

   public:
    explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

    MemTableIterator(const MemTableIterator&) = delete;
    MemTableIterator& operator=(const MemTableIterator&) = delete;

    ~MemTableIterator() override = default;

    bool Valid() const override { return iter_.Valid(); }

    void Seek(const std::string_view& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }

    void SeekToFirst() override { iter_.SeekToFirst(); }

    void SeekToLast() override { iter_.SeekToLast(); }

    void Next() override { iter_.Next(); }

    void Prev() override { iter_.Prev(); }

    // 解析长度前缀格式，返回用户 key
    std::string_view key() const override { return GetLengthPrefixedSlice(iter_.key()); }

    std::string_view value() const override {
        // 先解析 key，再跳过 key 解析 value
        std::string_view key_slice = GetLengthPrefixedSlice(iter_.key());
        return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
    }

    Status status() const override { return Status::OK(); }
};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

/**
 * entry 存储格式
┌────────────┬─────────────┬────────────┬────────────┬─────────────┐
│ key_size   │ key bytes   │ tag        │ value_size │ value bytes │
│ varint32   │ char[N]     │ uint64     │ varint32   │ char[M]     │
└────────────┴─────────────┴────────────┴────────────┴─────────────┘
                                      │
                                      └─ tag = (sequence << 8) | type
 */
void MemTable::Add(SequenceNumber s, ValueType type, const std::string_view& key, const std::string_view& value) {
    size_t key_size = key.size();
    size_t val_size = value.size();
    size_t internal_key_size = key_size + 8;  // 8 是 tag 长度
    const size_t encoded_len =
        VarintLength(internal_key_size) + internal_key_size + VarintLength(val_size) + val_size;  // 计算编码后的总长度
    char* buf = arena_.Allocate(encoded_len);                                                     // 分配内存

    char* p = EncodeVarint32(buf, internal_key_size);  // 写入 key 长度

    std::memcpy(p, key.data(), key_size);  // 赋值 key 部分
    p += key_size;
    EncodeFixed64(p, (s << 8) | type);  // 赋值 tag 部分
    p += 8;
    p = EncodeVarint32(p, val_size);  // 写入 value 长度

    std::memcpy(p, value.data(), val_size);  // 赋值 value 部分

    assert(p + val_size == buf + encoded_len);
    table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
    std::string scratch;
    std::string_view internal_key = key.internal_key();
    // MemTable 条目用 internal_key_size 前缀，而非 LookupKey 的 total_size 前缀
    const char* kptr = EncodeKey(&scratch, internal_key);

    Table::Iterator iter(&table_);
    iter.Seek(kptr);  // 定位

    if (iter.Valid()) {
        const char* entry = iter.key();
        uint32_t key_length;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
        std::string_view found_user_key(key_ptr, key_length - 8);
        int cmp = comparator_.comparator.user_comparator()->Compare(found_user_key, key.user_key());

        if (cmp == 0) {
            // user key 匹配
            const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
            switch (static_cast<ValueType>(tag & 0xff)) {
                case kTypeValue: {
                    std::string_view v = GetLengthPrefixedSlice(key_ptr + key_length);
                    value->assign(v.data(), v.size());
                    return true;
                }
                case kTypeDeletion: {
                    // 已删除
                    *s = Status::NotFound(std::string_view());
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace delta