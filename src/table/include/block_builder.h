#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <string_view>

namespace delta {

/**
 * @brief 定义了数据块（Block）构建器的接口，用于将多个 key-value 对打包成一个数据块，是 SSTable 的基本组成单元
 */
// ============================================================================
// block_builder.h - 数据块构建器接口
// ============================================================================
// 作用：构建 SSTable 中的数据块（Data Block）
//
// 核心功能：
//   1. 将多个 key-value 对打包成连续的二进制块
//   2. 使用前缀压缩减少 key 的存储空间
//   3. 支持重启点（Restart Point）加速查找
//
// Block 数据格式：
//   ┌─────────────────────────────────────────────────────────────┐
//   │  entry 1: (shared_len, non_shared_len, value_len,           │
//   │              non_shared_key_prefix, value)                  │
//   │  entry 2: (shared_len, non_shared_len, value_len,           │
//   │              non_shared_key_prefix, value)                  │
//   │  ...                                                        │
//   │  restart_point_1: uint32 (offset)                           │
//   │  restart_point_2: uint32 (offset)                           │
//   │  ...                                                        │
//   │  num_restarts: uint32                                       │
//   └─────────────────────────────────────────────────────────────┘
//
// 前缀压缩：
//   - shared_len: 与前一个 key 的公共前缀长度
//   - non_shared_key_prefix: 去除公共前缀后的 key 后缀
//
// 重启点：
//   - 每隔 block_restart_interval 个 entry 设置一个重启点
//   - 重启点处的 entry 不压缩前缀（shared_len = 0）
//   - 查找时可以二分查找重启点数组，定位到目标重启点, 从重启点开始线性扫描，最多扫描 block_restart_interval 个 entry
// ============================================================================

class BlockBuilder {
   private:
    //    格式：[entry0][entry1]...[entryN][restarts数组][num_restarts]
    std::string buffer_;              // 数据缓冲区（存储所有 entry 的二进制数据）
    std::vector<uint32_t> restarts_;  // 重启点数组（存储每个重启点的偏移量）
    int counter_;                     // 自上次重启点后已添加的 entry 数量，达到 block_restart_interval 时创建新重启点
    bool finished_;                   // Finish() 是否已被调用
    std::string last_key_;            // 上一个添加的 key （用于计算公共前缀）

   public:
    explicit BlockBuilder();

    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    /**
     * @brief 重置构建器，清空所有内容，从而可以复用 BlockBuilder
     * Finish() 后必须调用 Reset() 才能继续使用 BlockBuilder
     */
    void Reset();

    /**
     * @brief 添加 key-value 到块
     * key 必须大于之前添加的所有 key（按字典序）。
     * 压缩策略：
     *  - 非重启点：存储与前一个 key 的公共前缀长度 + 后缀。
     *  - 重启点：存储完整 key（shared_len = 0）
     */
    void Add(const std::string_view& key, const std::string_view& value);

    /**
     * @brief 完成块的构建
     * @return 指向块内容的 slice
     */
    std::string_view Finish();

    /**
     * @brief 估计当前块大小
     * @return 当前未压缩块大小的估计值
     */
    size_t CurrentSizeEstimate() const;

    /**
     * @brief 检查是否为空
     */
    bool empty() const { return buffer_.empty(); }
};

}  // namespace delta