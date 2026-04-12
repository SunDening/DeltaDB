#pragma once

#include <cstdint>

#include <deltadb/utils/iterator.h>

namespace delta {

/**
 * SST 表（Sorted String Table）的公共接口文件
 *
 * SST 文件的物理结构：
    ┌─────────────────────────────────────────────────────────┐
    │  Data Block 0                                           │
    │  (键值对数据)                                            │
    ├─────────────────────────────────────────────────────────┤
    │  Data Block 1                                           │
    ├─────────────────────────────────────────────────────────┤
    │  ...                                                    │
    ├─────────────────────────────────────────────────────────┤
    │  Data Block N                                           │
    ├─────────────────────────────────────────────────────────┤
    │  Meta Index Block (元数据索引)                            │
    │  - filter block 位置                                     │
    │  - 其他元数据                                            │
    ├─────────────────────────────────────────────────────────┤
    │  Index Block (数据块索引)                                 │
    │  - 每个 Data Block 的起始位置和最大键                      │
    ├─────────────────────────────────────────────────────────┤
    │  Footer (固定 48 字节)                                    │
    │  - Meta Index Block 位置                                 │
    │  - Index Block 位置                                      │
    │  - Magic Number                                          │
    └─────────────────────────────────────────────────────────┘
 *
    footer -> metaindex_block -> filter_block
    footer -> index_block -> data_block
 */

class Block;        // 数据块（基本存储单元）
class BlockHandle;  // 块的指针（偏移 + 大小）
class Footer;       // 文件尾部元数据
class RandomAccessFile;
class TableCache;

struct ReadOptions;

class Table {
   private:
    friend class SSTCache;

    /**
     * Req 包含：
     *  - RandomAccessFile* file - 底层文件
     *  - Block* index_block - 索引块
     *  - Block* filter_block - 过滤器块（可选）
     *  - BlockHandle filter_handle - 过滤器块位置
     *  - Comparator* comparator - 比较器
     *  - size_t cache_id - 缓存 ID
     */
    struct Rep;

    Rep* const rep_;

    /**
     * @brief 将索引迭代器的值 (编码的 BlockHandle) 转换为对应数据块的迭代器
     * @param arg Table 对象指针
     * @param options 读取选项
     * @param index_value 索引值 (编码的 BlockHandle)
     * @return 数据块迭代器
     *
     * 这是两层迭代器的关键：
     * - 外层：索引块迭代器
     * - 内层：数据块迭代器 (由此函数创建)
     */
    static Iterator* BlockReader(void* arg, const ReadOptions& options, const std::string_view& index_value);

    explicit Table(Rep* rep) : rep_(rep) {}

    /**
     * @brief 内部 Get 实现，用于单点查询
     * @param options 读取选项
     * @param k 查询键
     * @param arg 结果处理函数的参数
     * @param handle_result 结果处理回调
     * @return 状态码
     *
     * 优化：使用布隆过滤器快速判断键是否存在
     */
    Status InternalGet(const ReadOptions&, const std::string_view& key, void* arg,
                       void (*handle_result)(void* arg, const std::string_view& k, const std::string_view& v));

    /**
     * @brief 读取元索引块(metaindex_block)中的元数据，主要是布隆过滤器
     *
     * 元索引块包含:
     * - filter.<FilterPolicyName>: 布隆过滤器的句柄
     */
    void ReadMeta(const Footer& footer);

    /**
     * @brief 从元索引块中读取并解析布隆过滤器
     * @param filter_handle_value 过滤器块的编码句柄
     */
    void ReadFilter(const std::string_view& filter_handle_value);

   public:
    /**
     * @brief 打开一个 SSTable 文件
     * @param file 随机访问文件对象
     * @param size 文件大小
     * @param table 输出的 Table 指针
     * @return 状态码
     *
     * SSTable 文件结构 (从尾部开始):
     * [数据块 0][数据块 1]...[元索引块][索引块][Footer(固定 48 字节)]
     */
    static Status OpenSST(RandomAccessFile* file, uint64_t file_size, Table** table);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    ~Table();

    /**
     * @brief 创建一个新的迭代器用于遍历 SSTable
     * @param options 读取选项
     * @return 两层迭代器 (索引块迭代器 + 数据块迭代器)
     */
    Iterator* NewIterator(const ReadOptions&) const;

    /**
     * @brief 计算给定键在文件中的近似偏移量
     * @param key 查询键
     * @return 近似字节偏移量
     *
     * 用途：用于计算范围的大小 (如 Compaction 时估算数据量)
     */
    uint64_t ApproximateOffsetOf(const std::string_view& key) const;
};

}  // namespace delta