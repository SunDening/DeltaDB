#pragma once

#include <deltadb/db/write_batch.h>
#include <deltadb/utils/dbformat.h>

namespace delta {

class MemTable;

/**
 * 一个工具类，提供对 WriteBatch 内部状态的访问和操作接口。
 */
class WriteBatchInternal {
   public:
    // 获取批次中的操作数量
    static int Count(const WriteBatch* batch);

    // 设置批次中的操作数量
    static void SetCount(WriteBatch* batch, int n);

    // 获取起始序列号
    static SequenceNumber Sequence(const WriteBatch* batch);

    // 设置起始序列号
    static void SetSequence(WriteBatch* batch, SequenceNumber seq);

    // 获取原始二进制内容
    static std::string_view Contents(const WriteBatch* batch) { return std::string_view(batch->rep_); }

    // 获取原始二进制大小
    static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }

    // 从外部数据恢复批次内容
    static void SetContents(WriteBatch* batch, const std::string_view& contents);

    // 插入到 MemTable
    static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

    // 合并批次
    static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace delta