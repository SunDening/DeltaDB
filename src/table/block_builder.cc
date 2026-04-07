#include <algorithm>
#include <cassert>

#include "block_builder.h"
#include "comparator.h"
#include "coding.h"
#include "config.h"

namespace delta {

extern delta::Config::ptr gDBConfig;

/**
    键值对条目格式：
    ┌──────────────┬────────────────┬──────────────┬─────────────┬───────────┐
    │ shared_bytes │ unshared_bytes │ value_length │ key_delta   │ value     │
    │  (varint32)  │   (varint32)   │  (varint32)  │ (字节序列)  │ (字节序列) │
    └──────────────┴────────────────┴──────────────┴─────────────┴───────────┘

    块尾部（Trailer）格式：
    ┌─────────────────────────────┬────────────────┐
    │ restarts[num_restarts]      │ num_restarts   │
    │ (uint32 数组，每个 4 字节)    │ (uint32)       │
    └─────────────────────────────┴────────────────┘
 */

BlockBuilder::BlockBuilder() : restarts_(), counter_(0), finished_(false) {
    assert(gDBConfig->block_restart_internal >= 1);
    restarts_.push_back(0);  // 第一个重启点位于偏移量 0
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    finished_ = false;
    last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    // 原始数据 + 重启点数组 + 重启点数量字段
    return (buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t));
}

std::string_view BlockBuilder::Finish() {
    // 追加重启点数组
    for (size_t i=0; i<restarts_.size(); i++) {
        PutFixed32(&buffer_, restarts_[i]);
    }
    // 写入重启点数量
    PutFixed32(&buffer_, restarts_.size());
    // 标记块为完成状态
    finished_ = true;
    return std::string_view(buffer_);
}

void BlockBuilder::Add(const std::string_view& key, const std::string_view& value) {
    std::string_view last_key_piece(last_key_);
    assert(!finished_);
    assert(counter_ <= gDBConfig->block_restart_internal);
    // 确保键按字典序排列
    assert(buffer_.empty() || gDBConfig->comparator->Compare(key, last_key_piece) > 0);

    size_t shared = 0;
    if (counter_ < gDBConfig->block_restart_internal) {
        // 计算与前一个键的共享前缀长度
        const size_t min_length = std::min(last_key_piece.size(), key.size());
        while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
            shared++;
        }
    } else {
        // 到达重启点，记录新重启点位置并重置计数器
        restarts_.push_back(buffer_.size());
        counter_ = 0;
    }
    // 非共享键长度
    const size_t non_shared = key.size() - shared;

    // 写入条目头：shared + non_shared + value_length
    PutVarint32(&buffer_, shared);
    PutVarint32(&buffer_, non_shared);
    PutVarint32(&buffer_, value.size());

    // 写入键的非共享部分和值
    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    // 更新 last_key 为当前完整键
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    assert(std::string_view(last_key_) == key);
    counter_++;
}

}
