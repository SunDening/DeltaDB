#include <assert.h>

#include "coding.h"
#include "filter_block.h"
#include "filter_policy.h"

namespace delta {

// 每 2KB 数据生成一个 Filter（而非每个数据块一个）
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

// ============================================================================
// FilterBlockBuilder 实现
// ============================================================================

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}

void FilterBlockBuilder::GenerateFilter() {
    const size_t num_keys = start_.size();

    // 没有 key，生成空 filter
    if (num_keys == 0) {
        filter_offsets_.push_back(result_.size());
        return;
    }

    // 从扁平化的 keys_ 缓冲区重建 key 列表
    start_.push_back(keys_.size());  // 添加末尾哨兵，简化长度计算
    tmp_keys_.resize(num_keys);
    for (size_t i = 0; i < num_keys; i++) {
        const char* base = keys_.data() + start_[i];
        size_t length = start_[i + 1] - start_[i];
        tmp_keys_[i] = std::string_view(base, length);
    }

    // 调用 FilterPolicy 生成 Filter 并追加到 result_
    filter_offsets_.push_back(result_.size());
    policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

    // 清空缓冲区，准备下一组
    tmp_keys_.clear();
    keys_.clear();
    start_.clear();
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    // 计算 block_offset 所属的 Filter 索引
    uint64_t filter_index = (block_offset / kFilterBase);

    // 确保 filter_index 单调递增
    assert(filter_index >= filter_offsets_.size());

    // 如果 index 增长，生成中间的 Filter
    //  例如：从 index=0 跳到 index=2，需要生成 index=0 和 index=1 的 Filter
    while (filter_index > filter_offsets_.size()) {
        GenerateFilter();
    }
}

void FilterBlockBuilder::AddKey(const std::string_view& key) {
    std::string_view k = key;
    start_.push_back(keys_.size());
    keys_.append(k.data(), k.size());
}

std::string_view FilterBlockBuilder::Finish() {
    // 如果还有未生成的 key，生成最后一个 Filter
    if (!start_.empty()) {
        GenerateFilter();
    }

    // 追加偏移量数组
    const uint32_t array_offset = result_.size();
    for (size_t i = 0; i < filter_offsets_.size(); i++) {
        PutFixed32(&result_, filter_offsets_[i]);
    }

    // 追加元数据（5 字节）
    PutFixed32(&result_, array_offset);  // 偏移量数组位置（4 字节）
    result_.push_back(kFilterBaseLg);    // 编码参数（1 字节，存 log 值而非实际大小）
    return std::string_view(result_);
}

// ============================================================================
// FilterBlockReader 实现
// ============================================================================

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy, const std::string_view& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
    size_t n = contents.size();

    // 最小长度检查：至少需要 5 字节（1 字节 base_lg + 4 字节 array_offset）
    if (n < 5) return;
    // 读取编码参数（最后1字节）
    base_lg_ = contents[n - 1];

    // 读取偏移量数组的起始位置（倒数第 5~2 字节）
    uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
    if (last_word > n - 5) return;

    // 设置指针
    data_ = contents.data();      // filter 数据区起始
    offset_ = data_ + last_word;  // 偏移量数组起始

    // 计算 filter 数量
    // 偏移量数组每个元素 4 字节，数组长度 = (数组起始 - 数据起始 - 5) / 4
    num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const std::string_view& key) {
    // 计算 block_offset 属于哪个 filter
    //     右移 11 位 = 除以 2048
    uint64_t index = block_offset >> base_lg_;

    // 检查索引是否越界
    if (index < num_) {
        // 从偏移量数组读取 filter 的起始和结束位置
        uint32_t start = DecodeFixed32(offset_ + index * 4);
        uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);

        // 验证 filter 位置并提取数据
        if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
            std::string_view filter = std::string_view(data_ + start, limit - start);
            return policy_->KeyMayMatch(key, filter);
        } else if (start == limit) {
            // 空 filter，不匹配任何 key
            return false;
        }
    }

    // 错误情况（索引越界或数据损坏）：保守返回 true
    return true;
}

}  // namespace delta