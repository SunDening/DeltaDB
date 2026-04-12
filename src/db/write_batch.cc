#include <deltadb/db/memtable.h>
#include <deltadb/db/write_batch.h>
#include <deltadb/db/write_batch_internal.h>
#include <deltadb/utils/dbformat.h>

namespace delta {

/**
 * WriteBatch 的序列化格式
    WriteBatch::rep_ :=
        sequence: fixed64          # 8 字节：起始序列号
        count: fixed32             # 4 字节：操作数量
        data: record[count]        # 操作记录序列

    record :=
        kTypeValue varstring varstring   |  # Put 操作：类型 + key + value
        kTypeDeletion varstring          # Delete 操作：类型 + key

    varstring :=
        len: varint32               # 变长整数表示长度
        data: uint8[len]            # 实际数据
 *
 * Header 结构(12 字节)
 *  +----------------+----------------+
    |  sequence(8B)  |  count(4B)     |
    +----------------+----------------+
 *
 */

static const size_t kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::~WriteBatch() = default;

WriteBatch::Handler::~Handler() = default;

void WriteBatch::Clear() {
    rep_.clear();
    rep_.resize(kHeader);  // 预留 header 空间
}

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

/**
 * 遍历操作。遍历过程：
 *  1. 验证头部完整性（至少 12 字节）
 *  2. 跳过 12 字节头部
 *  3. 逐条解析记录：
 *      - 读取 1 字节类型标记
 *      - 根据类型解析 key/value
 *      - 调用对应的 handler 回调
 *  4. 验证解析的记录数与 header 中的 count 一致
 */
Status WriteBatch::Iterate(Handler* handler) const {
    std::string_view input(rep_);
    // 检查头部完整性
    if (input.size() < kHeader) {
        return Status::Corruption("malformat WriteBatch (too small)");
    }

    // 跳过 12 字节头部
    input.remove_prefix(kHeader);

    std::string_view key, value;
    int found = 0;  // 实际解析到的记录数

    // 逐条解析
    while (!input.empty()) {
        found++;
        char tag = input[0];  // 类型标记，1字节
        input.remove_prefix(1);

        switch (tag) {
            case kTypeValue:
                // 解析长度前缀的 key 和 value
                // GetLengthPrefixedSlice: 先读 varint32 长度，再创建对应长度的 Slice
                if (GetLengthPrefixedSlice(&input, &key) && GetLengthPrefixedSlice(&input, &value)) {
                    handler->Put(key, value);  // 回调用户处理器
                } else {
                    return Status::Corruption("bad WriteBatch Put");
                }
                break;
            case kTypeDeletion:
                if (GetLengthPrefixedSlice(&input, &key)) {
                    handler->Delete(key);
                } else {
                    return Status::Corruption("bad WriteBatch Delete");
                }
                break;
            default:
                return Status::Corruption("unknown WriteBatch tag");
        }
    }
    // 验证解析的记录数与 header 中存储的 count 一致
    if (found != WriteBatchInternal::Count(this)) {
        return Status::Corruption("WriteBatch has wrong count");
    } else {
        return Status::OK();
    }
}

/**
 * 获取批次中的操作数量
 * 读取位置：rep_[8:12] (偏移 8 字节处的 4 字节)
 * 编码方式：Fixed32 (小端序)
 */
int WriteBatchInternal::Count(const WriteBatch* b) { return DecodeFixed32(b->rep_.data() + 8); }

/**
 * 设置批次中的操作数量
 * 写入位置：rep_[8:12]
 */
void WriteBatchInternal::SetCount(WriteBatch* b, int n) { EncodeFixed32(&b->rep_[8], n); }

/**
 * 获取批次的起始序列号
 * 读取位置：rep_[0:8] (头部前 8 字节)
 * 用途：
 *  - 确保批量写入的原子性（所有操作共享连续序列号）
 *  - WAL 恢复时重建 MemTable
 */
SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
    return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

/**
 * 设置批次的起始序列号
 * 写入位置：rep_[0:8]
 * 调用时机：DB::Write() 提交前分配序列号
 */
void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) { EncodeFixed64(&b->rep_[0], seq); }

/**
 * 添加一个 Put 操作到批次末尾
 * 序列化格式：
 * +--------+-----------+-----------+
 * | tag(1) | key(var)  | value(var)|
 * +--------+-----------+-----------+
 * 步骤：
 *  1. count++ （更新头部计数）
 *  2. 写入 1 字节类型标记 kTypeValue
 *  3. 写入长度前缀的 key  (varint32_len + data)
 *  4. 写入长度前缀的 value (varint32_len + data)
 */
void WriteBatch::Put(const std::string_view& key, const std::string_view& value) {
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}

/**
 * 添加一个 Delete 操作到批次末尾
 * 序列化格式：
 * +--------+-----------+
 * | tag(1) | key(var)  |
 * +--------+-----------+
 *
 */
void WriteBatch::Delete(const std::string_view& key) {
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeDeletion));
    PutLengthPrefixedSlice(&rep_, key);
}

/**
 * 将另一个批次的操作追加到当前批次
 * 直接复制二进制数据，避免重复编解码
 */
void WriteBatch::Append(const WriteBatch& source) { WriteBatchInternal::Append(this, &source); }

namespace {

/**
 * 内部类，将 WriteBatch 写入 MemTable
 * 工作原理：
 *  - 持有起始序列号和 MemTable 指针
 *  - 每处理一个操作，序列号自动递增
 *  - 调用 MemTable::Add() 实际写入
 */
class MemTableInserter : public WriteBatch::Handler {
   public:
    SequenceNumber sequence_;
    MemTable* mem_;

    void Put(const std::string_view& key, const std::string_view& value) override {
        mem_->Add(sequence_, kTypeValue, key, value);
        sequence_++;
    }
    void Delete(const std::string_view& key) override {
        mem_->Add(sequence_, kTypeDeletion, key, std::string_view());
        sequence_++;
    }
};

}  // namespace

/**
 * 将 WriteBatch 中的所有操作插入到 MemTable
 */
Status WriteBatchInternal::InsertInto(const WriteBatch* bat, MemTable* memtable) {
    MemTableInserter inserter;                               // 创建 MemTableInserter 实例
    inserter.sequence_ = WriteBatchInternal::Sequence(bat);  // 设置起始序列号（从 WriteBatch 头部读取）
    inserter.mem_ = memtable;                                // 设置目标 MemTable
    return bat->Iterate(&inserter);                          // 遍历并插入
}

/**
 * 从外部二进制数据（通常来自 WAL 日志）恢复 WriteBatch 内容
 */
void WriteBatchInternal::SetContents(WriteBatch* bat, const std::string_view& contents) {
    assert(contents.size() >= kHeader);
    bat->rep_.assign(contents.data(), contents.size());  // 复制数据
}

/**
 * 将源批次的操作追加到目标批次
 */
void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
    // 更新计数：dst.count += src.count
    SetCount(dst, Count(dst) + Count(src));
    assert(src->rep_.size() >= kHeader);
    // 直接追加 src 的数据区（跳过 12 字节头部）
    dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace delta