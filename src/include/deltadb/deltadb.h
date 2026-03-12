#pragma once
#include <memory>
#include <string>

namespace deltadb {

class Iterator;
class WriteBatch;
struct Options;
struct ReadOptions;
struct WriteOptions;

class DB {
   public:
    virtual ~DB() = default;

    // 基础操作
    virtual bool Put(const WriteOptions& opt, const std::string& key, const std::string& value) = 0;

    virtual bool Get(const ReadOptions& opt, const std::string& key, std::string* value) = 0;

    virtual bool Delete(const WriteOptions& opt, const std::string& key) = 0;

    // 原子批量写
    virtual bool Write(const WriteOptions& opt, WriteBatch* batch) = 0;

    // 范围查询
    virtual Iterator* NewIterator(const ReadOptions& opt) = 0;

    // 工厂方法
    static bool Open(const Options& options, const std::string& name, DB** dbptr);
};

}  // namespace deltadb