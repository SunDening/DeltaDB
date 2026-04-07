#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace delta {

/**
 * 用于加速 SST 文件中的点查询（Point Lookup），通过 Bloom Filter 快速判断某个 key 是否 不
 存在于数据块中，从而避免不必要的磁盘 I/O
 *
 * 问题场景：
 *  - LevelDB 的 SST 文件包含多个数据块（Data Block）
 *  - DB::Get(key) 查询时，需要确定 key 在哪个数据块
 *  - 如果没有过滤器，可能需要读取多个数据块才能确定 key 不存在
 * 解决方案：
 *  - 为每个数据块建立一个 Bloom Filter
 *  - 查询前先检查 Bloom Filter
 *  - 如果 Filter 说"不存在"，则肯定不存在，无需读盘
 *  - 如果 Filter 说"可能存在"，才实际读取数据块
 *
    Filter Block 格式 (位于 SST 文件末尾):
    ┌─────────────────────────────────────────────────────────┐
    │  Filter 0  │  Filter 1  │  ...  │  Filter N-1  │  meta  │
    │ (块 0 过滤器)│ (块 1 过滤器)│       │ (块 N-1 过滤器)│    │
    └─────────────────────────────────────────────────────────┘
                                                            ↑
                                                offset_array + meta

    meta (5 字节):
    ┌──────────────┬──────────────┬──────────────┬──────────────┬───────┐
    │ offset[0]    │ offset[1]    │     ...      │ offset[N-1]  │ base  │
    │ (4 字节)      │ (4 字节)      │              │ (4 字节)    │ _lg   │
    └──────────────┴──────────────┴──────────────┴──────────────┴───────┘
             ↑                                                    ↑
        指向 Filter 0 的起始位置                            编码参数 (11)
 *
 */

class FilterPolicy;

/**
 * 增量构建。 为 SST 文件中的所有数据块生成过滤器，并将它们合并存储为一个特殊的块
 * 用于构建 SST 文件中单个 Filter Block 的类
 */
class FilterBlockBuilder {
   private:
    const FilterPolicy* policy_;
    std::string keys_;                        // 扁平化的 key 内容缓冲区
    std::vector<size_t> start_;               // 每个 key 在 key_ 中的起始位置
    std::string result_;                      // 已生成的 Filter 数据
    std::vector<std::string_view> tmp_keys_;  // 临时参数
    std::vector<uint32_t> filter_offsets_;    // 每个 Filter 在 result_ 中的偏移量

    // 生成当前累积 keys 的 Filter 并追加到 result
    void GenerateFilter();

   public:
    explicit FilterBlockBuilder(const FilterPolicy*);

    FilterBlockBuilder(const FilterBlockBuilder&) = delete;
    FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

    /**
     * @brief 声明一个新数据块的开始
     * @param block_offset: 数据块在 SST 文件中的起始偏移量
     *
     * 根据 offset 计算所属的 Filter 索引（每 2KB 数据生成一个 Filter）
     * 如果索引增长，则生成之前的 Filter
     */
    void StartBlock(uint64_t block_offset);

    /**
     * @brief 将 key 追加到 keys_ 缓冲区，等待后续生成 Filter
     * @param key：要添加的键
     */
    void AddKey(const std::string_view& key);

    /**
     * @brief 完成构建，返回 Filter Block 数据
     *
     * 生成的数据格式：[Filter 0][Filter 1]...[Filter N-1][offset 数组][meta]
     */
    std::string_view Finish();
};

/**
 * 用于读取和查询 Filter Block 的类
 * 从 SST 文件中读取 Filter Block，并提供快速的 KeyMayMatch 查询
 */
class FilterBlockReader {
   private:
    const FilterPolicy* policy_;
    const char* data_;    // 指向 Filter 数据区起始位置
    const char* offset_;  // 指向偏移量数组起始位置（块末尾）
    size_t num_;          // 偏移量数组中的条目数（即 Filter 数量）
    size_t base_lg_;      // 编码参数（2 的幂次，默认 11 表示每 2KB 一个 Filter）

   public:
    /**
     * @param contents：Filter Block 的完整数据
     */
    FilterBlockReader(const FilterPolicy* policy, const std::string_view& contents);

    /**
     * @brief 检查 key 是否可能存在于指定偏移量的数据块中
     * @param block_offset：数据块在 SST 文件中的起始偏移量
     * @param key：要查询的键
     */
    bool KeyMayMatch(uint64_t block_offset, const std::string_view& key);
};

}  // namespace delta