#include <cstdint>

#include "block.h"
#include "coding.h"
#include "comparator.h"
#include "sst_format.h"

namespace delta {

// ============================================================================
// Block: LevelDB SST 文件中的数据块类
// ============================================================================
// 数据块格式:
//   [entry1][entry2]...[entryN][restart1][restart2]...[restartM][num_restarts]
//
//   - entry: KV 条目，使用前缀压缩存储
//   - restart: 重启点数组，每个重启点是一个偏移量 (4 字节)
//   - num_restarts: 重启点数量 (4 字节，位于块末尾)
//
// entry 格式:
//   [shared_bytes][non_shared_bytes][value_length][key_delta][value]
//   - shared_bytes: 与前一条 key 共享的前缀字节数
//   - non_shared_bytes: 非共享的 key 后缀字节数
//   - value_length: value 长度
//   - key_delta: 非共享的 key 后缀
//   - value: 实际值
// ============================================================================

/**
 * @brief 从块末尾的 4 字节读取重启点的数量
 */
inline uint32_t Block::RestartsNum() const {
    assert(size_ >= sizeof(uint32_t));
    return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()), size_(contents.data.size()), owned_(contents.heap_allocated) {
    // 检查块大小是否合法
    if (size_ < sizeof(uint32_t)) {
        size_ = 0;  // 块太小，无法存储重启点数量
    } else {
        // 计算允许的最大重启点数量
        size_t max_restart_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
        // 验证重启点数量是否合理
        if (RestartsNum() > max_restart_allowed) {
            size_ = 0;  // 重启点数量超过块大小允许的范围，数据损坏
        } else {
            // 计算重启点数组的起始偏移量
            restart_offset_ = size_ - (1 + RestartsNum()) * sizeof(uint32_t);
        }
    }
}

Block::~Block() {
    // 如果数据是堆分配的，负责释放
    if (owned_) {
        delete[] data_;
    }
}

/**
 * @brief 解码单个 entry 的头部信息
 * @param ptr：当前 entry 的起始位置
 * @param limit：数据区边界（重启点数组之前）
 * @param shared：输出参数，与前一条 key 共享的前缀字节数
 * @param non_shared：输出参数，非共享的 key 后缀字节数
 * @param value_length：输出参数，value 长度
 * @return 成功：指向 key_delta 的指针；失败：nullptr
 */
static inline const char* DecodeEntry(const char* ptr, const char* limit, uint32_t* shared, uint32_t* non_shared,
                                      uint32_t* value_length) {
    // 检查是否有足够的字节读取三个长度值
    if (limit - ptr < 3) {
        return nullptr;
    }
    // 读取三个长度值
    *shared = reinterpret_cast<const uint8_t*>(ptr)[0];
    *non_shared = reinterpret_cast<const uint8_t*>(ptr)[1];
    *value_length = reinterpret_cast<const uint8_t*>(ptr)[2];
    if ((*shared | *non_shared | *value_length) < 128) {
        // 快速路径：三个值都 < 128，说明没有使用 varint 编码
        ptr += 3;
    } else {
        // 慢速路径：至少有一个值 >= 128，使用 varint 编码
        if ((ptr = GetVarint32Ptr(ptr, limit, shared)) == nullptr) return nullptr;
        if ((ptr = GetVarint32Ptr(ptr, limit, non_shared)) == nullptr) return nullptr;
        if ((ptr = GetVarint32Ptr(ptr, limit, value_length)) == nullptr) return nullptr;
    }

    // 检查剩余数据是否足够容纳 key 后缀和 value
    if (static_cast<uint32_t>(limit - ptr) < (*non_shared + *value_length)) {
        return nullptr;
    }
    return ptr;
}

/**
 * @brief Block 的迭代器实现
 */
class Block::Iter : public Iterator {
   private:
    const Comparator* const comparator_;  // key 比较器
    const char* const data_;              // 底层数据块指针
    uint32_t const restarts_;             // 重启点数组的偏移量
    uint32_t const restarts_num_;         // 重启点数量

    uint32_t current_;        // 当前 entry 的偏移量
    uint32_t restart_index_;  // current_ 所在的重启点索引
    std::string key_;         // 当前 key （解压后的完整 key）
    std::string_view value_;  // 当前 value
    Status status_;           // 错误状态

    /**
     * @brief 比较两个 key
     */
    inline int Compare(const std::string_view& akey, const std::string_view& bkey) const {
        return comparator_->Compare(akey, bkey);
    }

    /**
     * @brief 返回当前 entry 结束位置的下一个字节偏移量
     */
    inline uint32_t NextEntryOffset() const { return (value_.data() + value_.size()) - data_; }

    /**
     * @brief 获取指定索引的重启点偏移量
     */
    uint32_t GetRestartPoint(uint32_t index) {
        assert(index < restarts_num_);
        return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
    }

    /**
     * @brief 定位到指定的重启点
     */
    void SeekToRestartPoint(uint32_t index) {
        key_.clear();
        restart_index_ = index;

        // ParseNextKey() 从 value_ 末尾开始解析，所以设置 value_ 为重启点位置
        uint32_t offset = GetRestartPoint(index);
        value_ = std::string_view(data_ + offset, 0);
    }

    /**
     * @brief 标记数据损坏错误
     */
    void CorruptionError() {
        current_ = restarts_;  // 设为无效位置
        restart_index_ = restarts_num_;
        status_ = Status::Corruption("bad entry in block");
        key_.clear();
        value_ = {};
    }

    /**
     * @brief 解析下一条 key-value 记录
     *
     * 工作原理：
     *  从 value_ 的末尾开始（即上一个 entry 的结束位置），解码下一个 entry。
     *  由于 key 使用前缀压缩，需要与前一条 key 合并得到完整 key
     */
    bool ParseNextKey() {
        // 设置当前 entry 的起始位置
        current_ = NextEntryOffset();
        const char* p = data_ + current_;
        const char* limit = data_ + restarts_;  // 重启点数组是数据区边界

        // 检查是否到达数据区末尾
        if (p >= limit) {
            // 没有更多 entry，标记为无效
            current_ = restarts_;
            restart_index_ = restarts_num_;
            return false;
        }

        // 解码 entry 头部
        uint32_t shared, non_shared, value_length;
        p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);

        if (p == nullptr || key_.size() < shared) {
            // 解码失败或 shared 超过当前 key 的长度，数据损坏
            CorruptionError();
            return false;
        } else {
            // 重建完整 key：前缀（shared）+ 后缀（non_shared）
            key_.resize(shared);
            key_.append(p, non_shared);

            // 设置 value
            value_ = std::string_view(p + non_shared, value_length);

            // 更新 restart_index_，确保它指向包含 current_ 的重启点区间
            while (restart_index_ + 1 < restarts_num_ && GetRestartPoint(restart_index_ + 1) < current_) {
                restart_index_++;
            }
            return true;
        }
    }

   public:
    Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t restarts_num)
        : comparator_(comparator),
          data_(data),
          restarts_(restarts),
          restarts_num_(restarts_num),
          current_(restarts_),
          restart_index_(restarts_num_) {
        assert(restarts_num_ > 0);
    }

    /**
     * @brief 检查迭代器是否有效
     */
    bool Valid() const override { return current_ < restarts_; }

    Status status() const override { return status_; }

    std::string_view key() const override {
        assert(Valid());
        return key_;
    }
    std::string_view value() const override {
        assert(Valid());
        return value_;
    }

    /**
     * @brief 移动到下一条记录
     */
    void Next() override {
        assert(Valid());
        ParseNextKey();
    }

    /**
     * @brief 移动到前一条记录
     */
    void Prev() override {
        assert(Valid());

        // 向后扫描，找到 current_ 之前的重启点
        const uint32_t original = current_;
        while (GetRestartPoint(restart_index_) >= original) {
            if (restart_index_ == 0) {
                // 已经是第一个重启点，无法再向前
                current_ = restarts_;
                restart_index_ = restarts_num_;
                return;
            }
            restart_index_--;
        }

        // 定位到重启点
        SeekToRestartPoint(restart_index_);
        do {
            // 循环直到当前entry的结束位置 >= original
        } while (ParseNextKey() && NextEntryOffset() < original);
    }

    /**
     * @brief 定位到第一条 key >= target 的记录
     */
    void Seek(const std::string_view& target) override {
        // 二分查找重启点数组，找到最后一个 key < target 的重启点
        uint32_t left = 0;
        uint32_t right = restarts_num_ - 1;
        int current_key_compare = 0;

        if (Valid()) {
            current_key_compare = Compare(key_, target);
            if (current_key_compare < 0) {
                // key_ < target，目标在当前位置之后
                left = restart_index_;
            } else if (current_key_compare > 0) {
                // key_ > target，目标在当前位置之前
                right = restart_index_;
            } else {
                // key_ == target，已经找到
                return;
            }
        } else {
        }

        // 二分查找
        while (left < right) {
            uint32_t mid = (left + right + 1) / 2;
            uint32_t region_offset = GetRestartPoint(mid);

            // 读取重启点处的 entry 信息
            uint32_t shared, non_shared, value_length;
            const char* key_ptr =
                DecodeEntry(data_ + region_offset, data_ + restarts_, &shared, &non_shared, &value_length);
            if (key_ptr == nullptr || (shared != 0)) {
                CorruptionError();
                return;
            }
            std::string_view mid_key(key_ptr, non_shared);
            if (Compare(mid_key, target) < 0) {
                left = mid;
            } else {
                right = mid - 1;
                ;
            }
        }

        // 检查是否可以跳过 Seek （目标已在当前重启点且位于当前位置之后）
        assert(current_key_compare == 0 || Valid());
        bool skip_seek = left == restart_index_ && current_key_compare < 0;
        if (!skip_seek) {
            SeekToRestartPoint(left);
        }

        while (true) {
            if (!ParseNextKey()) {
                return;
            }
            if (Compare(key_, target) >= 0) {
                return;
            }
        }
    }

    /**
     * @brief 定位到第一条记录
     */
    void SeekToFirst() override {
        SeekToRestartPoint(0);  // 第一个重启点
        ParseNextKey();
    }

    /**
     * @brief 定位到最后一条记录
     */
    void SeekToLast() override {
        SeekToRestartPoint(restarts_num_ - 1);
        while (ParseNextKey() && NextEntryOffset() < restarts_) {
            // 一直跳过直到块末尾
        }
    }
};

/**
 * @brief 创建 Block 的迭代器
 */
Iterator* Block::NewIterator(const Comparator* comparator) {
    // 检查块大小是否合法
    if (size_ < sizeof(uint32_t)) {
        return NewErrorIterator(Status::Corruption("bad block contents"));
    }

    const uint32_t restarts_num = RestartsNum();

    if (restarts_num == 0) {
        return NewEmptyIterator();
    } else {
        return new Iter(comparator, data_, restart_offset_, restarts_num);
    }
}

}  // namespace delta