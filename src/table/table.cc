#include <deltadb/table/block.h>
#include <deltadb/table/cache.h>
#include <deltadb/table/filter_block.h>
#include <deltadb/table/random_access_file.h>
#include <deltadb/table/sst_format.h>
#include <deltadb/table/table.h>
#include <deltadb/table/two_level_iterator.h>
#include <deltadb/utils/comparator.h>
#include <deltadb/utils/filter_policy.h>

namespace delta {

extern delta::Config::ptr gDBConfig;

/**
 * @brief Table 的内部实现细节 (Pimpl 模式)
 */
struct Table::Rep {
    Status status;              // 状态信息
    RandomAccessFile* file;     // 底层文件指针
    uint64_t cache_id;          // 缓存标识符，用于区分不同表的缓存块
    FilterBlockReader* filter;  // 布隆过滤器读取器
    const char* filter_data;    // 过滤器原始数据指针

    // footer -> metaindex_block -> filter_block
    // footer -> index_block -> data_block
    BlockHandle metaindex_handle;  // 元索引块的句柄（从 footer 保存）
    Block* index_block;            // 索引块（指向数据块的索引）

    ~Rep() {
        delete filter;
        delete[] filter_data;
        delete index_block;
    }
};

Status Table::OpenSST(RandomAccessFile* file, uint64_t size, Table** table) {
    *table = nullptr;

    // 文件大小必须至少能容纳 Footer
    if (size < Footer::kEncodedLength) {
        return Status::Corruption("file is too short to be an sst");
    }

    // 读取 Footer （文件末尾的固定长度元数据）
    char footer_space[Footer::kEncodedLength];
    std::string_view footer_input;
    Status status = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input, footer_space);
    if (!status.ok()) return status;

    // 解析 Footer，获取索引块和元索引块的位置
    // footer -> metaindex_block -> filter_block
    // footer -> index_block -> data_block
    Footer footer;
    status = footer.DecodeFrom(&footer_input);
    if (!status.ok()) return status;

    // 读取索引块
    BlockContents index_block_contents;
    ReadOptions opt;
    if (gDBConfig->paranoid_checks) {  // 严格检查模式
        // 验证数据校验和
        opt.verify_checksums = true;
    }
    status = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);

    if (status.ok()) {
        // 创建 SST 实例
        Block* index_block = new Block(index_block_contents);
        Rep* rep = new Table::Rep;
        rep->file = file;
        rep->metaindex_handle = footer.metaindex_handle();
        rep->index_block = index_block;
        rep->cache_id = (gDBConfig->block_cache ? gDBConfig->block_cache->NewId() : 0);
        rep->filter_data = nullptr;
        rep->filter = nullptr;
        *table = new Table(rep);

        // 读取元数据（元索引块）
        (*table)->ReadMeta(footer);
    }
    return status;
}

void Table::ReadMeta(const Footer& footer) {
    if (gDBConfig->filter_policy == nullptr) {
        // 没有配置过滤器，也就不用读元索引块了
        return;
    }

    ReadOptions opt;
    if (gDBConfig->paranoid_checks) {  // 严格检查模式
        // 验证数据校验和
        opt.verify_checksums = true;
    }

    // 读取元索引块
    BlockContents contents;
    if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
        // 元数据不是必需的，错误不传播
        return;
    }
    Block* meta = new Block(contents);

    // 在元索引块中查找过滤器
    Iterator* iter = meta->NewIterator(BytewiseComparator());
    // 构造过滤器键名
    std::string key = "filter.";
    key.append(gDBConfig->filter_policy->Name());
    iter->Seek(key);
    if (iter->Valid() && iter->key() == std::string_view(key)) {
        // 找到匹配的过滤器，读取它
        ReadFilter(iter->value());
    }
    delete iter;
    delete meta;
}

void Table::ReadFilter(const std::string_view& filter_handle_value) {
    std::string_view v = filter_handle_value;
    BlockHandle filter_handle;
    // 解码过滤器块的句柄
    if (!filter_handle.DecodeFrom(&v).ok()) {
        return;
    }

    ReadOptions opt;
    if (gDBConfig->paranoid_checks) {
        opt.verify_checksums = true;
    }

    // 读取过滤器块
    BlockContents block;
    if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
        return;
    }

    // 保存过滤器数据
    if (block.heap_allocated) {
        rep_->filter_data = block.data.data();  // 记录需要释放的数据
    }

    // 创建过滤器读取器
    rep_->filter = new FilterBlockReader(gDBConfig->filter_policy, block.data);
}

Table::~Table() { delete rep_; }

static void DeleteBlock(void* arg, void* /*ignored*/) { delete reinterpret_cast<Block*>(arg); }

static void DeleteCachedBlock(const std::string_view& /*key*/, void* value) {
    Block* block = reinterpret_cast<Block*>(value);
    delete block;
}

static void ReleaseBlock(void* arg, void* h) {
    Cache* cache = reinterpret_cast<Cache*>(arg);
    Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
    cache->Release(handle);
}

Iterator* Table::BlockReader(void* arg, const ReadOptions& options, const std::string_view& index_value) {
    Table* table = reinterpret_cast<Table*>(arg);
    Cache* block_cache = gDBConfig->block_cache;
    Block* block = nullptr;
    Cache::Handle* cache_handle = nullptr;

    // 解码 BlockHandle
    BlockHandle handle;
    std::string_view input = index_value;
    Status status = handle.DecodeFrom(&input);

    if (status.ok()) {
        BlockContents contents;
        if (block_cache != nullptr) {
            // 使用缓存：构造缓存键 (cache_id + block_offset)
            char cache_key_buffer[16];
            EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
            EncodeFixed64(cache_key_buffer + 8, handle.offset());
            std::string_view key(cache_key_buffer, sizeof(cache_key_buffer));

            // 查找缓存
            cache_handle = block_cache->Lookup(key);
            if (cache_handle != nullptr) {
                block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
            } else {
                status = ReadBlock(table->rep_->file, options, handle, &contents);
                if (status.ok()) {
                    block = new Block(contents);
                    if (contents.cacheable && options.fill_cache) {
                        cache_handle = block_cache->Insert(key, block, block->size(), &DeleteCachedBlock);
                    }
                }
            }
        } else {
            status = ReadBlock(table->rep_->file, options, handle, &contents);
            if (status.ok()) {
                block = new Block(contents);
            }
        }
    }

    // 创建块迭代器并注册清理回调
    Iterator* iter;
    if (block != nullptr) {
        iter = block->NewIterator(gDBConfig->internal_comparator);
        if (cache_handle == nullptr) {
            iter->RegisterCleanup(&DeleteBlock, block, nullptr);
        } else {
            iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
        }
    } else {
        iter = NewErrorIterator(status);
    }
    return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
    return NewTwoLevelIterator(rep_->index_block->NewIterator(gDBConfig->internal_comparator), &Table::BlockReader,
                               const_cast<Table*>(this), options);
}

Status Table::InternalGet(const ReadOptions& options, const std::string_view& k, void* arg,
                          void (*handle_result)(void*, const std::string_view&, const std::string_view&)) {
    Status status;
    // 在索引块中查找目标键所在的块
    Iterator* iiter = rep_->index_block->NewIterator(gDBConfig->internal_comparator);
    iiter->Seek(k);

    if (iiter->Valid()) {
        std::string_view handle_value = iiter->value();
        FilterBlockReader* filter = rep_->filter;
        BlockHandle handle;

        // 布隆过滤器优化：如果过滤器说键不存在，直接跳过
        if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() && !filter->KeyMayMatch(handle.offset(), k)) {
            // Not found (布隆过滤器确定键不存在)
        } else {
            // 读取数据块并查找
            Iterator* block_iter = BlockReader(this, options, iiter->value());

            // 遍历数据块中的所有键（调试）
            for (block_iter->SeekToFirst(); block_iter->Valid(); block_iter->Next()) {
            }

            block_iter->Seek(k);
            if (block_iter->Valid()) {
                (*handle_result)(arg, block_iter->key(), block_iter->value());
            } else {
            }
            status = block_iter->status();
            if (!status.ok()) {
            }
            delete block_iter;
        }
    } else {
    }
    if (status.ok()) {
        status = iiter->status();
    }
    delete iiter;
    return status;
}

uint64_t Table::ApproximateOffsetOf(const std::string_view& key) const {
    Iterator* index_iter = rep_->index_block->NewIterator(gDBConfig->internal_comparator);
    index_iter->Seek(key);
    uint64_t result;

    if (index_iter->Valid()) {
        BlockHandle handle;
        std::string_view input = index_iter->value();
        Status status = handle.DecodeFrom(&input);
        if (status.ok()) {
            result = handle.offset();  // 返回数据块的偏移量
        } else {
            // 无法解码：返回元索引块偏移量 (接近文件末尾)
            result = rep_->metaindex_handle.offset();
        }
    } else {
        // 键超出文件范围：返回元索引块偏移量
        result = rep_->metaindex_handle.offset();
    }
    delete index_iter;
    return result;
}

}  // namespace delta