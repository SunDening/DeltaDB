#include "sst_cache.h"
#include "filename.h"
#include "random_access_file.h"

namespace delta {

/**
 * 将 RandomAccessFile 和 Table 绑定在一起作为一个缓存单元
 * 因为 Table 需要底层文件支持，两者生命周期相同
 */
struct TableAndFile {
    RandomAccessFile* file;  // 底层文件对象
    Table* table;            // SSTable 抽象对象
};

/**
 * @brief 缓存条目删除时的清理函数
 * @param key：缓存键（file_number 的编码）
 * @param value：TableAndFile* 的指针
 */
static void DeleteEntry(const std::string_view& /*key*/, void* value) {
    TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
    delete tf->table;
    delete tf->file;
    delete tf;
}

/**
 * @brief 迭代器清理时的引用释放函数。减少缓存条目的引用计数，而不是直接删除
 * @param arg1：Cache* 指针
 * @param arg2：Cache::Handle* 句柄
 */
static void UnrefEntry(void* arg1, void* arg2) {
    Cache* cache = reinterpret_cast<Cache*>(arg1);
    Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
    cache->Release(h);
}

SSTCache::SSTCache(const std::string& dbname, int entries_num) : dbname_(dbname), cache_(NewLRUCache(entries_num)) {}

SSTCache::~SSTCache() { delete cache_; }

Status SSTCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
    Status status;

    // 构造缓存键：将 file_number 编码为 8 字节
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    std::string_view key(buf, sizeof(buf));

    // 查找缓存
    *handle = cache_->Lookup(key);

    if (*handle == nullptr) {
        // 缓存未命中，需要打开文件
        std::string fname = SSTFileName(dbname_, file_number);
        RandomAccessFile* file = nullptr;
        Table* table = nullptr;

        // 尝试打开文件（仅使用 .sst 后缀）
        status = NewRandomAccessFile(fname, &file);

        // 创建 Table 对象
        if (status.ok()) {
            status = Table::OpenSST(file, file_size, &table);
        }

        if (!status.ok()) {
            // 失败处理。不缓存错误结果
            assert(table == nullptr);
            delete file;
        } else {
            // 成功，加入缓存
            TableAndFile* tf = new TableAndFile;
            tf->file = file;
            tf->table = table;
            // 插入缓存，value 大小为 1 (用于 LRU 容量计算)
            // DeleteEntry 会在条目被驱逐时自动调用
            *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
        }
    }
    return status;
}

Iterator* SSTCache::NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                                Table** table_ptr) {
    if (table_ptr != nullptr) {
        *table_ptr = nullptr;
    }

    // 查找或打开文件
    Cache::Handle* handle = nullptr;
    Status status = FindTable(file_number, file_size, &handle);
    if (!status.ok()) {
        return NewErrorIterator(status);  // 返回错误迭代器
    }

    // 从缓存值中获取 Table 对象
    Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;

    // 创建 Table 迭代器
    Iterator* result = table->NewIterator(options);

    // 注册清理回调：迭代器销毁时释放缓存引用
    result->RegisterCleanup(&UnrefEntry, cache_, handle);

    if (table_ptr != nullptr) {
        *table_ptr = table;
    }
    return result;
}

Status SSTCache::Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const std::string_view& k,
                     void* arg, void (*handle_result)(void*, const std::string_view&, const std::string_view&)) {
    Cache::Handle* handle = nullptr;

    // 查找或打开文件
    Status status = FindTable(file_number, file_size, &handle);
    if (status.ok()) {
        // 获取 Table 对象并执行查询
        Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
        status = t->InternalGet(options, k, arg, handle_result);

        // 释放缓存引用（FindTable 增加了引用计数）
        cache_->Release(handle);
    }
    return status;
}

void SSTCache::Evict(uint64_t file_number) {
    // 构造缓存键 (与 FindTable 一致)
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);

    // 从缓存中删除. Erase 会触发 DeleteEntry 清理函数，删除 Table 和 File 对象
    cache_->Erase(std::string_view(buf, sizeof(buf)));
}

}  // namespace delta