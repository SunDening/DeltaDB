#include <crc32c/crc32c.h>

#include <deltadb/db/filename.h>
#include <deltadb/db/version_edit.h>
#include <deltadb/table/block_builder.h>
#include <deltadb/table/filter_block.h>
#include <deltadb/table/sst_builder.h>
#include <deltadb/table/sst_cache.h>
#include <deltadb/table/sst_format.h>
#include <deltadb/utils/coding.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/utils/filter_policy.h>
#include <deltadb/utils/iterator.h>
#include <deltadb/wal/writable_file.h>

namespace delta {

Status BuildSST(const std::string& dbname, SSTCache* sst_cache, Iterator* iter, SSTMetaData* sst) {
    Status status;
    sst->sst_size = 0;

    // 定位到第一个数据
    iter->SeekToFirst();

    // 生成 SSTable 文件名
    std::string sst_name = SSTFileName(dbname, sst->sst_number);

    // 如果有数据，开始构建 SSTable
    if (iter->Valid()) {
        WritableFile* file;
        status = NewWritableFile(sst_name, &file);
        if (!status.ok()) {
            return status;
        }

        // 创建 SSTBuilder 负责 SSTable 的格式构建
        SSTBuilder* builder = new SSTBuilder(file);

        // 记录最小 key（第一个数据的 key）
        sst->smallest_key.DecodeFrom(iter->key());

        // 遍历所有数据，写入 SSTable
        std::string_view key;
        for (; iter->Valid(); iter->Next()) {
            key = iter->key();
            builder->Add(key, iter->value());
        }

        // 记录最大 key （最后一个数据的 key）
        if (!key.empty()) {
            sst->largest_key.DecodeFrom(key);
        }

        // 完成构建（写入索引块、元索引块、Footer）
        status = builder->Finish();
        if (status.ok()) {
            sst->sst_size = builder->FileSize();  // 获取文件大小
            assert(sst->sst_size > 0);
        }
        delete builder;

        // 同步文件到磁盘
        if (status.ok()) {
            status = file->Sync();
        }

        // 关闭文件
        if (status.ok()) {
            status = file->Close();
        }
        delete file;
        file = nullptr;

        // 验证表的可用性（以缓存打开新迭代器，检查状态）
        if (status.ok()) {
            Iterator* it = sst_cache->NewIterator(ReadOptions(), sst->sst_number, sst->sst_size);
            status = it->status();
            delete it;
        }
    }

    // 检查输入迭代器是否有错误
    if (!iter->status().ok()) {
        status = iter->status();
    }

    // 如果失败或文件大小为0，删除文件
    if (status.ok() && sst->sst_size > 0) {
        // 成功，保留文件
    } else {
        RemoveFile(sst_name);
    }

    return status;
}

// ================================ SSTBuilder class ================================
// ============================================================================
// SSTable 构建器实现
// ============================================================================
// 作用：实现 SSTBuilder 类，负责构建 SSTable 文件
//
// SSTable 结构：
//   [Data Blocks]      ← 存储实际数据
//   [Filter Block]     ← 布隆过滤器（可选）
//   [Meta Index Block] ← 元数据索引
//   [Index Block]      ← 数据块索引
//   [Footer]           ← 固定 48 字节，包含 Index 和 Meta Index 的位置
// ============================================================================

struct SSTBuilder::Rep {
    WritableFile* file;        // 输出文件
    uint64_t offset;           // 当前文件写入偏移量
    Status status;             // 错误状态
    BlockBuilder data_block;   // 数据块构建器
    BlockBuilder index_block;  // 索引块构建器
    std::string last_key;
    int64_t entries_num;               // 条目计数
    bool closed;                       // 是否已关闭
    FilterBlockBuilder* filter_block;  // 过滤快构建器
    bool pending_index_entry;          // 是否有待处理的索引项
    BlockHandle pending_handle;
    std::string compressed_output;

    Rep(WritableFile* f)
        : file(f),
          offset(0),
          data_block(),
          index_block(),
          entries_num(0),
          closed(false),
          filter_block(gDBConfig->filter_policy == nullptr ? nullptr
                                                           : new FilterBlockBuilder(gDBConfig->filter_policy)),
          pending_index_entry(false) {
        gDBConfig->block_restart_internal = 1;
    }
};

SSTBuilder::SSTBuilder(WritableFile* file) : rep_(new Rep(file)) {
    if (rep_->filter_block != nullptr) {
        // 启动第一个过滤器块
        rep_->filter_block->StartBlock(0);
    }
}

SSTBuilder::~SSTBuilder() {
    assert(rep_->closed);  // 捕获调用者忘记调用 Finish() 的错误
    delete rep_->filter_block;
    delete rep_;
}

void SSTBuilder::Add(const std::string_view& key, const std::string_view& value) {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;
    if (r->entries_num > 0) {
        // 确保键单调递增（使用 Internal Key Comparator，因为 key 是内部键）
        assert(gDBConfig->internal_comparator->Compare(key, std::string_view(r->last_key)) > 0);
    }

    // 如果有待处理的索引项，先写入索引
    if (r->pending_index_entry) {
        assert(r->data_block.empty());
        // 使用最短分隔符作为索引键（优化索引空间）
        gDBConfig->internal_comparator->FindShortestSeparator(&r->last_key, key);
        std::string handle_encoding;
        r->pending_handle.EncodeTo(&handle_encoding);
        r->index_block.Add(r->last_key, std::string_view(handle_encoding));
        r->pending_index_entry = false;
    }

    // 添加到过滤器块
    if (r->filter_block != nullptr) {
        r->filter_block->AddKey(key);
    }

    // 更新状态并添加到数据块
    r->last_key.assign(key.data(), key.size());
    r->entries_num++;
    r->data_block.Add(key, value);

    // 如果数据块达到阈值，刷新到文件
    const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
    if (estimated_block_size >= gDBConfig->block_size) {
        Flush();
    }
}

void SSTBuilder::Flush() {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;
    if (r->data_block.empty()) return;
    assert(!r->pending_index_entry);

    // 写入数据块，获取块句柄（偏移量 + 大小）
    WriteBlock(&r->data_block, &r->pending_handle);
    if (ok()) {
        r->pending_index_entry = true;
        r->status = r->file->Flush();
    }

    // 启动新的过滤器块
    if (r->filter_block != nullptr) {
        r->filter_block->StartBlock(r->offset);
    }
}

void SSTBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
    assert(ok());
    Rep* r = rep_;
    std::string_view raw = block->Finish();  // 获取块的原始数据

    std::string_view block_content;
    CompressionType type = gDBConfig->compression;

    // 根据压缩类型处理
    switch (type) {
        case kNoCompression:
            block_content = raw;
            break;
        case kSnappyCompression: {
            std::string* compressed = &r->compressed_output;
            // 只有压缩后小于原大小的 87.5% 才使用压缩
            if (Snappy_Compress(raw.data(), raw.size(), compressed) &&
                compressed->size() < raw.size() - (raw.size() / 8u)) {
                block_content = *compressed;
            } else {
                block_content = raw;
                type = kNoCompression;
            }
            break;
        }
        case kZstdCompression: {
            std::string* compressed = &r->compressed_output;
            if (Zstd_Compress(gDBConfig->zstd_compression_level, raw.data(), raw.size(), compressed) &&
                compressed->size() < raw.size() - (raw.size() / 8u)) {
                block_content = *compressed;
            } else {
                block_content = raw;
                type = kNoCompression;
            }
            break;
        }
    }
    WriteRawBlock(block_content, type, handle);
    r->compressed_output.clear();
    block->Reset();  // 重置块构建器（复用）
}

void SSTBuilder::WriteRawBlock(const std::string_view& block_contents, CompressionType type, BlockHandle* handle) {
    Rep* r = rep_;
    handle->set_offset(r->offset);            // 设置块偏移量
    handle->set_size(block_contents.size());  // 设置块大小

    r->status = r->file->Append(block_contents);  // 写入块数据

    if (r->status.ok()) {
        // 写入尾部：1 字节压缩类型 + 4 字节CRC校验码
        char trailer[kBlockTrailerSize];
        trailer[0] = type;
        uint32_t crc = crc32c::Crc32c(block_contents.data(), block_contents.size());
        crc = crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(trailer), 1);
        EncodeFixed32(trailer + 1, crc);
        r->status = r->file->Append(std::string_view(trailer, kBlockTrailerSize));
        if (r->status.ok()) {
            r->offset += block_contents.size() + kBlockTrailerSize;
        }
    }
}

Status SSTBuilder::status() const { return rep_->status; }

Status SSTBuilder::Finish() {
    Rep* r = rep_;
    Flush();
    assert(!r->closed);
    r->closed = true;

    BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

    // 写入过滤器块
    if (ok() && r->filter_block != nullptr) {
        WriteRawBlock(r->filter_block->Finish(), kNoCompression, &filter_block_handle);
    }

    //  写入元索引块（包含过滤器快的位置信息）
    if (ok()) {
        BlockBuilder metaindex_block;
        if (r->filter_block != nullptr) {
            std::string key = "filter.";
            key.append(gDBConfig->filter_policy->Name());
            std::string handle_encoding;
            filter_block_handle.EncodeTo(&handle_encoding);
            metaindex_block.Add(key, handle_encoding);
        }

        WriteBlock(&metaindex_block, &metaindex_block_handle);
    }

    // 写入索引块
    if (ok()) {
        // 处理最后一个待处理的索引项
        if (r->pending_index_entry) {
            gDBConfig->internal_comparator->FindShortSuccessor(&r->last_key);
            std::string handle_encoding;
            r->pending_handle.EncodeTo(&handle_encoding);
            r->index_block.Add(r->last_key, std::string_view(handle_encoding));
            r->pending_index_entry = false;
        }
        WriteBlock(&r->index_block, &index_block_handle);
    }

    // 写入 Footer（包含元索引块和索引块的句柄）
    if (ok()) {
        Footer footer;
        footer.set_metaindex_handle(metaindex_block_handle);
        footer.set_index_handle(index_block_handle);
        std::string footer_encoding;
        footer.EncodeTo(&footer_encoding);
        r->status = r->file->Append(footer_encoding);
        if (r->status.ok()) {
            r->offset += footer_encoding.size();
        }
    }
    return r->status;
}

void SSTBuilder::Abandon() {
    Rep* r = rep_;
    assert(!r->closed);
    r->closed = true;
}

uint64_t SSTBuilder::EntriesNum() const { return rep_->entries_num; }

uint64_t SSTBuilder::FileSize() const { return rep_->offset; }

}  // namespace delta