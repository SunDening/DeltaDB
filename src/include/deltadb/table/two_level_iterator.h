#pragma once

#include <deltadb/utils/config.h>
#include <deltadb/utils/iterator.h>

namespace delta {

/**
 * 两级迭代器，用于遍历 SST 文件中的所有 KV 数据。
 * 它将“索引块迭代器”和“数据块迭代器”结合起来，对外提供统一的迭代接口
 *
    SST 文件:
    ┌─────────────────────────────────────────┐
    │ 数据块 0 [k1, k2, k3...]                 │
    │ 数据块 1 [k4, k5, k6...]                 │
    │ 数据块 2 [k7, k8, k9...]                 │
    │ ...                                     │
    ├─────────────────────────────────────────┤
    │ 索引块 [(k3→块 0), (k6→块 1), (k9→块 2)] │  ← index_iter
    └─────────────────────────────────────────┘
 *  TwoLevelIterator 自动在数据块之间切换，用户无需关心块的存在
 */

/**
 * @brief 创建两级迭代器
 * @param index_iter：索引迭代器（遍历索引块，每个 value 指向同一个数据块）
 * @param block_function：工厂函数，将索引值转换为数据块迭代器
 * @param arg：传递给 block_function 的参数
 * @param options：读取选项
 */
Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                                          const std::string_view& index_value),
                              void* arg, const ReadOptions& options);

}  // namespace delta