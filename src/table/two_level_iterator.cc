#include <deltadb/table/block.h>
#include <deltadb/table/sst_format.h>
#include <deltadb/table/table.h>
#include <deltadb/table/two_level_iterator.h>

#include <deltadb/table/iterator_wrapper.h>

namespace delta {

// 函数指针类型别名：将索引值转换为数据块迭代器的工厂函数
typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const std::string_view&);

class TwoLevelIterator : public Iterator {
   private:
    BlockFunction block_function_;       // 数据块迭代器工厂函数
    void* arg_;                          // 工厂函数的参数
    const ReadOptions options_;          // 读取选项
    Status status_;                      // 保存的错误状态
    delta::IteratorWrapper index_iter_;  // 索引迭代器（包装器）
    delta::IteratorWrapper data_iter_;   // 数据块迭代器
    std::string data_block_handle_;      // 当前数据块的句柄

    /**
     * @brief 保存错误状态（只保存第一个非 OK 状态）
     */
    void SaveError(const Status& s) {
        if (status_.ok() && !s.ok()) status_ = s;
    }

    /**
     * @brief 向前跳过空的数据块（用于 Next/Seek/SeekToFirst）
     */
    void SkipEmptyDataBlocksForward();

    /**
     * @brief 向后跳过空的数据块（用于 Prev/SeekToLast）
     */
    void SkipEmptyDataBlocksBackward();

    /**
     * @brief 设置数据块迭代器（接管所有权，保存旧迭代器的错误）
     */
    void SetDataIterator(Iterator* data_iter);

    /**
     * @brief 根据当前 index_iter_ 初始化 data_iter_
     */
    void InitDataBlock();

   public:
    // 工作流程:
    //   1. 通过 index_iter_ 定位到目标索引项（数据块句柄）
    //   2. 调用 block_function_ 创建数据块迭代器
    //   3. 通过 data_iter_ 遍历该数据块中的 KV
    //   4. 数据块遍历完后，自动切换到下一个数据块
    TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options);

    ~TwoLevelIterator() override;

    void Seek(const std::string_view& target) override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Next() override;
    void Prev() override;

    /**
     * @brief 判断迭代器是否有效（data_iter_ 有效即有效）
     */
    bool Valid() const override { return data_iter_.Valid(); }

    /**
     * @brief 返回当前 key（委托给 data_iter_）
     */
    std::string_view key() const override {
        assert(Valid());
        return data_iter_.key();
    }

    /**
     * @brief 返回当前 value（委托给 data_iter_）
     */
    std::string_view value() const override {
        assert(Valid());
        return data_iter_.value();
    }

    /**
     * @brief 返回当前错误状态
     */
    Status status() const override {
        // It'd be nice if status() returned a const Status& instead of a Status
        if (!index_iter_.status().ok()) {
            return index_iter_.status();  // index_iter_ 的错误（索引层错误）
        } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
            return data_iter_.status();  // data_iter_ 的错误（数据块层错误）
        } else {
            return status_;  // status_（保存的历史错误）
        }
    }
};

/**
 * @param index_iter：索引迭代器（被接管所有权）
 * @param block_function：工厂函数，根据索引值创建数据块迭代器
 * @param arg：传递给工厂函数的参数
 * @param options：读取选项
 */
TwoLevelIterator::TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function), arg_(arg), options_(options), index_iter_(index_iter), data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() = default;

/**
 * @brief 定位到第一条 key >= target 的记录
 */
void TwoLevelIterator::Seek(const std::string_view& target) {
    index_iter_.Seek(target);                                   // 在索引层定位到目标索引项
    InitDataBlock();                                            // 初始化数据块迭代器
    if (data_iter_.iter() != nullptr) data_iter_.Seek(target);  // 在数据块层定位
    SkipEmptyDataBlocksForward();                               // 跳过空的数据块
}

/**
 * @brief 定位到第一条记录
 */
void TwoLevelIterator::SeekToFirst() {
    index_iter_.SeekToFirst();                                   // 索引层定位到第一个索引项
    InitDataBlock();                                             // 初始化数据块迭代器
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();  // 数据块层定位到第一条记录
    SkipEmptyDataBlocksForward();                                // 跳过空的数据块
}

/**
 * @brief 定位到最后一条记录
 */
void TwoLevelIterator::SeekToLast() {
    index_iter_.SeekToLast();                                   // 索引层定位到最后一个索引项
    InitDataBlock();                                            // 初始化数据块迭代器
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();  // 数据块层定位到最后一条记录
    SkipEmptyDataBlocksBackward();                              // 跳过空的数据块（向后）
}

/**
 * @brief 移动到下一条记录
 */
void TwoLevelIterator::Next() {
    assert(Valid());
    data_iter_.Next();
    SkipEmptyDataBlocksForward();
}

/**
 * @brief 移动到前一条记录
 */
void TwoLevelIterator::Prev() {
    assert(Valid());
    data_iter_.Prev();
    SkipEmptyDataBlocksBackward();
}

/**
 * @brief 向前跳过空的数据块
 */
void TwoLevelIterator::SkipEmptyDataBlocksForward() {
    while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
        // 检查索引层是否耗尽
        if (!index_iter_.Valid()) {
            SetDataIterator(nullptr);
            return;
        }

        // 移动到下一个索引项
        index_iter_.Next();

        // 根据新索引项初始化数据块迭代器
        InitDataBlock();

        // 定位到数据块的第一条记录
        if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
    }
}

/**
 * @brief 向后跳过空的数据块
 */
void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
    while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
        // Move to next block
        if (!index_iter_.Valid()) {
            SetDataIterator(nullptr);
            return;
        }
        index_iter_.Prev();
        InitDataBlock();
        if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
    }
}

/**
 * @brief 设置数据块迭代器
 */
void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
    if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
    data_iter_.Set(data_iter);
}

/**
 * @brief 根据当前 index_iter_ 初始化 data_iter_
 */
void TwoLevelIterator::InitDataBlock() {
    if (!index_iter_.Valid()) {
        SetDataIterator(nullptr);
    } else {
        std::string_view handle = index_iter_.value();
        if (data_iter_.iter() != nullptr && handle.compare(data_block_handle_) == 0) {
            // data_iter_ is already constructed with this iterator, so
            // no need to change anything
        } else {
            Iterator* iter = (*block_function_)(arg_, options_, handle);
            data_block_handle_.assign(handle.data(), handle.size());
            SetDataIterator(iter);
        }
    }
}

/**
 * @brief 工厂函数
 * @param index_iter：索引迭代器（被接管所有权）
 * @param block_function：工厂函数，根据索引值创建数据块迭代器
 * @param arg：传递给工厂函数的参数
 * @param options：读取选项
 */
Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
    return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace delta